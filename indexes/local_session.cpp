#include "local_session.h"
#include <map>

using namespace ioremap::elliptics;

/* matches above enum, please update synchronously */
static const char *update_index_action_strings[] = {
	"empty",
	"insert",
	"remove",
};

static int noop_process(struct dnet_net_state *, struct epoll_event *) { return 0; }

#undef list_entry
#define list_entry(ptr, type, member) ({			\
	const list_head *__mptr = (ptr);	\
	(dnet_io_req *)( (char *)__mptr - offsetof(dnet_io_req, member) );})

#undef list_for_each_entry_safe
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_entry((head)->next, decltype(*pos), member),	\
		n = list_entry(pos->member.next, decltype(*pos), member);	\
	     &pos->member != (head); 					\
	     pos = n, n = list_entry(n->member.next, decltype(*n), member))

local_session::local_session(dnet_node *node) : m_flags(DNET_IO_FLAGS_CACHE)
{
	m_state = reinterpret_cast<dnet_net_state *>(malloc(sizeof(dnet_net_state)));
	if (!m_state)
		throw std::bad_alloc();

	dnet_addr addr;
	memset(&addr, 0, sizeof(addr));

	memset(m_state, 0, sizeof(dnet_net_state));

	m_state->need_exit = 1;
	m_state->write_s = -1;
	m_state->read_s = -1;

	dnet_state_micro_init(m_state, node, &addr, 0, noop_process);
	dnet_state_get(m_state);
}

local_session::~local_session()
{
	dnet_state_put(m_state);
	dnet_state_put(m_state);
}

void local_session::set_ioflags(uint32_t flags)
{
	m_flags = flags;
}

data_pointer local_session::read(const dnet_id &id, int *errp)
{
	return read(id, NULL, NULL, errp);
}

data_pointer local_session::read(const dnet_id &id, uint64_t *user_flags, dnet_time *timestamp, int *errp)
{
	dnet_io_attr io;
	memset(&io, 0, sizeof(io));
	dnet_empty_time(&io.timestamp);

	memcpy(io.id, id.id, DNET_ID_SIZE);
	memcpy(io.parent, id.id, DNET_ID_SIZE);

	io.flags = DNET_IO_FLAGS_NOCSUM | m_flags;

	dnet_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));

	cmd.id = id;
	cmd.cmd = DNET_CMD_READ;
	cmd.flags |= DNET_FLAGS_NOLOCK;
	cmd.size = sizeof(io);

	int err = dnet_process_cmd_raw(m_state, &cmd, &io, 0);
	if (err) {
		clear_queue();
		*errp = err;
		return data_pointer();
	}

	struct dnet_io_req *r, *tmp;

	list_for_each_entry_safe(r, tmp, &m_state->send_list, req_entry) {
		dnet_log(m_state->n, DNET_LOG_DEBUG, "hsize: %zu, dsize: %zu\n", r->hsize, r->dsize);

		dnet_cmd *req_cmd = reinterpret_cast<dnet_cmd *>(r->header ? r->header : r->data);

		dnet_log(m_state->n, DNET_LOG_DEBUG, "entry in list, status: %d\n", req_cmd->status);

		if (req_cmd->status) {
			*errp = req_cmd->status;
			clear_queue();
			return data_pointer();
		} else if (req_cmd->size) {
			dnet_io_attr *req_io = reinterpret_cast<dnet_io_attr *>(req_cmd + 1);

			if (user_flags)
				*user_flags = req_io->user_flags;
			if (timestamp)
				*timestamp = req_io->timestamp;

			dnet_log(m_state->n, DNET_LOG_DEBUG, "entry in list, size: %llu\n",
				static_cast<unsigned long long>(req_io->size));

			data_pointer result;

			if (r->data) {
				result = data_pointer::copy(r->data, r->dsize);
			} else {
				result = data_pointer::allocate(req_io->size);
				ssize_t read_res = pread(r->fd, result.data(), result.size(), r->local_offset);
				if (read_res == -1) {
					*errp = errno;
					clear_queue();
					return data_pointer();
				}
			}


			clear_queue();
			return result;
		}
	}

	*errp = -ENOENT;
	clear_queue();
	return data_pointer();
}

int local_session::write(const dnet_id &id, const data_pointer &data)
{
	return write(id, data.data<char>(), data.size());
}

int local_session::write(const dnet_id &id, const char *data, size_t size)
{
	dnet_time null_time;
	dnet_empty_time(&null_time);
	return write(id, data, size, 0, null_time);
}

int local_session::write(const dnet_id &id, const char *data, size_t size, uint64_t user_flags, const dnet_time &timestamp)
{
	dnet_io_attr io;
	memset(&io, 0, sizeof(io));
	dnet_empty_time(&io.timestamp);

	memcpy(io.id, id.id, DNET_ID_SIZE);
	memcpy(io.parent, id.id, DNET_ID_SIZE);
	io.flags |= DNET_IO_FLAGS_COMMIT | DNET_IO_FLAGS_NOCSUM | m_flags;
	io.size = size;
	io.num = size;
	io.user_flags = user_flags;
	io.timestamp = timestamp;

	dnet_current_time(&io.timestamp);

	data_buffer buffer(sizeof(dnet_io_attr) + size);
	buffer.write(io);
	buffer.write(data, size);

	dnet_log(m_state->n, DNET_LOG_DEBUG, "going to write size: %zu\n", size);

	data_pointer datap = std::move(buffer);

	dnet_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));

	cmd.id = id;
	cmd.cmd = DNET_CMD_WRITE;
	cmd.flags |= DNET_FLAGS_NOLOCK;
	cmd.size = datap.size();

	int err = dnet_process_cmd_raw(m_state, &cmd, datap.data(), 0);

	clear_queue(&err);

	return err;
}

data_pointer local_session::lookup(const dnet_cmd &tmp_cmd, int *errp)
{
	dnet_cmd cmd = tmp_cmd;
	cmd.flags |= DNET_FLAGS_NOLOCK;
	cmd.size = 0;

	*errp = dnet_process_cmd_raw(m_state, &cmd, NULL, 0);

	if (*errp)
		return data_pointer();

	struct dnet_io_req *r, *tmp;

	list_for_each_entry_safe(r, tmp, &m_state->send_list, req_entry) {
		dnet_cmd *req_cmd = reinterpret_cast<dnet_cmd *>(r->header ? r->header : r->data);

		if (req_cmd->status) {
			*errp = req_cmd->status;
			clear_queue();
			return data_pointer();
		} else if (req_cmd->size) {
			data_pointer result = data_pointer::copy(req_cmd + 1, req_cmd->size);
			clear_queue();
			return result;
		}
	}

	*errp = -ENOENT;
	clear_queue();
	return data_pointer();
}

int local_session::update_index_internal(const dnet_id &id, const dnet_raw_id &index, const data_pointer &data, update_index_action action)
{
	struct timeval start, end;

	gettimeofday(&start, NULL);

	data_buffer buffer(sizeof(dnet_indexes_request) + sizeof(dnet_indexes_request_entry) + data.size());

	dnet_indexes_request request;
	dnet_indexes_request_entry entry;
	memset(&request, 0, sizeof(request));
	memset(&entry, 0, sizeof(entry));

	request.id = id;
	request.entries_count = 1;

	buffer.write(request);

	entry.id = index;
	entry.size = data.size();
	entry.flags |= action;

	buffer.write(entry);
	if (!data.empty()) {
		buffer.write(data.data<char>(), data.size());
	}

	data_pointer datap = std::move(buffer);

	dnet_cmd cmd;
	memset(&cmd, 0, sizeof(cmd));
	memcpy(cmd.id.id, index.id, sizeof(cmd.id.id));

	cmd.cmd = DNET_CMD_INDEXES_INTERNAL;
	cmd.size = datap.size();

	int err = dnet_process_cmd_raw(m_state, &cmd, datap.data(), 0);

	clear_queue(&err);

	gettimeofday(&end, NULL);
	long diff = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);

	if (m_state->n->log->log_level >= DNET_LOG_INFO) {
		char index_str[2*DNET_ID_SIZE+1];

		dnet_dump_id_len_raw(index.id, 8, index_str);

		dnet_log(m_state->n, DNET_LOG_INFO, "%s: updating internal index: %s, data-size: %zd, action: %s, "
				"time: %ld usecs\n",
				dnet_dump_id(&id), index_str, data.size(), update_index_action_strings[action], diff);
	}

	return err;
}

void local_session::clear_queue(int *errp)
{
	struct dnet_io_req *r, *tmp;

	list_for_each_entry_safe(r, tmp, &m_state->send_list, req_entry) {
		dnet_cmd *cmd = reinterpret_cast<dnet_cmd *>(r->header ? r->header : r->data);

		if (errp && cmd->status)
			*errp = cmd->status;

		list_del(&r->req_entry);
		dnet_io_req_free(r);
	}
}
