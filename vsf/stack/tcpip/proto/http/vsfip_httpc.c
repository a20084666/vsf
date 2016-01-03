#include "app_type.h"
#include "compiler.h"

#include "interfaces.h"

#include "framework/vsfsm/vsfsm.h"
#include "framework/vsftimer/vsftimer.h"

#include "../../vsfip.h"

#include "vsfip_httpc.h"
#include "stack/tcpip/proto/dns/vsfip_dnsc.h"

#include <stdlib.h>

#ifdef HTTPC_DEBUG
#include "framework/vsfshell/vsfshell.h"
#endif

#define VSFIP_HTTPC_AGENT				"VSFIP"
#define VSFIP_HTTPC_SOCKET_TIMEOUT		4000

#define VSFSM_EVT_HTTPC_STREAM_OUT		(VSFSM_EVT_USER_LOCAL + 0x01)
#define VSFSM_EVT_HTTPC_STREAM_IN		(VSFSM_EVT_USER_LOCAL + 0x02)

static vsf_err_t
vsfip_httpc_prasewww(struct vsfip_httpc_param_t *httpc, char *www)
{
	//cut http head
	if(memcmp(www, "http://", sizeof("http://") - 1) == 0)
	{
		www += sizeof("http://") - 1;
	}

	httpc->host = www;
	httpc->file = strchr((char const *)www, '/');
	if (NULL == httpc->file)
	{
		httpc->file = "/";
	}

	return VSFERR_NONE;
}

static vsf_err_t vsfip_httpc_buildreq_get(struct vsfip_httpc_param_t *httpc,
									struct vsfip_buffer_t *buf)
{
	char *dst = (char *)buf->app.buffer, *pos_slash;

	strcpy(dst, "GET ");
	strcat(dst, (const char *)httpc->file);
	strcat(dst, " HTTP/1.1\r\n" "Host: ");
	pos_slash = strchr(httpc->host, '/');
	if (NULL == pos_slash)
	{
		strcat(dst, (const char *)httpc->host);
	}
	else
	{
		memcpy(dst, httpc->host, pos_slash - httpc->host);
	}
	strcat(dst, "\r\n" "Connection: keep-alive\r\n"\
				"User-Agent: " VSFIP_HTTPC_AGENT "\r\n\r\n");
	buf->app.size = strlen((const char *)dst);
	return VSFERR_NONE;
}

static vsf_err_t vsfip_httpc_prasehead(struct vsfip_httpc_param_t *httpc,
									struct vsfip_buffer_t *buf)
{
#define VSFIP_RESP_MATCH(ptr, str)		\
			!strncmp((const char *)(cur), (str), sizeof(str) - 1)

	uint8_t *cur = buf->app.buffer;
	uint8_t *end = buf->app.buffer + buf->app.size;

	if (!VSFIP_RESP_MATCH(cur, "HTTP/1.1 "))
	{
		return VSFERR_FAIL;
	}
	cur += sizeof("HTTP/1.1 ") - 1;

	httpc->resp_code = atoi((const char *)cur);
	if (httpc->resp_code != 200)
	{
		return VSFERR_FAIL;
	}

	cur = (uint8_t *)strchr((const char *)cur, '\n') + 1;
	while (cur < end)
	{
		if (VSFIP_RESP_MATCH(cur, "Content-"))
		{
			cur += sizeof("Content-") - 1;

			if (VSFIP_RESP_MATCH(cur, "Type: "))
			{
				cur += sizeof("Type: ") - 1;
				httpc->resp_type = cur;
			}
			else if (VSFIP_RESP_MATCH(cur, "Length: "))
			{
				cur += sizeof("Length: ") - 1;
				httpc->resp_length = atoi((char const *)cur);
			}
		}
		else if (cur[0] == '\r' && cur[1] == '\n')
		{
			cur += 2;
			buf->app.size -= cur - buf->app.buffer;
			buf->app.buffer = cur;
			return VSFERR_NONE;
		}

		cur = (uint8_t *)strchr((const char *)cur, '\n') + 1;
	}

	return VSFERR_NOT_READY;
}

vsf_err_t httpc_get(struct vsfsm_pt_t *pt, vsfsm_evt_t evt, char *wwwaddr,
									void *output)
{
	struct vsfip_httpc_param_t *httpc =
							(struct vsfip_httpc_param_t *)pt->user_data;
	vsf_err_t err = VSFERR_NONE, tcp_close_err = VSFERR_NONE;

	vsfsm_pt_begin(pt);

	err = vsfip_httpc_prasewww(httpc, wwwaddr);
	if (err) return err;

#ifdef HTTPC_DEBUG
	httpc->debug_pt.sm = pt->sm;
#endif

#ifdef HTTPC_DEBUG
	vsfshell_printf(&httpc->debug_pt, "->DNS %s" VSFSHELL_LINEEND, httpc->host);
#endif
	httpc->so->rx_timeout_ms = VSFIP_HTTPC_SOCKET_TIMEOUT;
	httpc->so->tx_timeout_ms = VSFIP_HTTPC_SOCKET_TIMEOUT;
	httpc->local_pt.sm = pt->sm;
	httpc->local_pt.state = 0;
	vsfsm_pt_entry(pt);
	err = vsfip_gethostbyname(&httpc->local_pt, evt, httpc->host,
								&httpc->hostip.sin_addr);
	if (err > 0) return err; else if (err < 0) return err;
#ifdef HTTPC_DEBUG
	vsfshell_printf(&httpc->debug_pt,
						"<-DNS GET %d.%d.%d.%d" VSFSHELL_LINEEND,
						httpc->hostip.sin_addr.addr.s_addr_buf[0],
						httpc->hostip.sin_addr.addr.s_addr_buf[1],
						httpc->hostip.sin_addr.addr.s_addr_buf[2],
						httpc->hostip.sin_addr.addr.s_addr_buf[3]);
#endif

	httpc->so = vsfip_socket(AF_INET, IPPROTO_TCP);
	if (httpc->so == NULL)
		return VSFERR_NOT_ENOUGH_RESOURCES;

	httpc->hostip.sin_port = httpc->port;
	httpc->local_pt.state = 0;
	vsfsm_pt_entry(pt);
	err = vsfip_tcp_connect(&httpc->local_pt, evt, httpc->so, &httpc->hostip);
	if (err > 0) return err; else if (err < 0)
	{
#ifdef HTTPC_DEBUG
		vsfshell_printf(&httpc->debug_pt, "->CONNECT FAIL" VSFSHELL_LINEEND);
#endif
		goto close;
	}

	httpc->buf = VSFIP_TCPBUF_GET(VSFIP_CFG_TCP_MSS);
	if (httpc->buf == NULL)
	{
#ifdef HTTPC_DEBUG
		vsfshell_printf(&httpc->debug_pt, "->NO BUF FAIL" VSFSHELL_LINEEND);
#endif
		err = VSFERR_NOT_ENOUGH_RESOURCES;
		goto tcp_close;
	}

	err = vsfip_httpc_buildreq_get(httpc, httpc->buf);
	if (err != 0)
	{
#ifdef HTTPC_DEBUG
		vsfshell_printf(&httpc->debug_pt, "->ERR REQ FAIL" VSFSHELL_LINEEND);
#endif
		goto tcp_close;
	}

	httpc->local_pt.state = 0;
	vsfsm_pt_entry(pt);
	err = vsfip_tcp_send(&httpc->local_pt, evt, httpc->so, NULL, httpc->buf, true);
	if (err > 0) return err; else if (err < 0)
	{
#ifdef HTTPC_DEBUG
		vsfshell_printf(&httpc->debug_pt, "->SEND REQ FAIL" VSFSHELL_LINEEND);
#endif
		goto tcp_close;
	}
	httpc->buf = NULL;
	httpc->so->tx_timeout_ms = 0;

#ifdef HTTPC_DEBUG
	vsfshell_printf(&httpc->debug_pt, "->SEND GET " VSFSHELL_LINEEND);
#endif

	httpc->resp_length = 0;
	httpc->resp_curptr = 0;
	while (1)
	{
		httpc->local_pt.state = 0;
		vsfsm_pt_entry(pt);
		err = vsfip_tcp_recv(&httpc->local_pt, evt, httpc->so, NULL,
							&httpc->buf);
		if (err > 0) return err; else if (err < 0)
		{
#ifdef HTTPC_DEBUG
			vsfshell_printf(&httpc->debug_pt, "->RECV FAIL" VSFSHELL_LINEEND);
#endif
			goto tcp_close;
		}

		if (0 == httpc->resp_length)
		{
#ifdef HTTPC_DEBUG
			vsfshell_printf(&httpc->debug_pt, "->RECV HEAD " VSFSHELL_LINEEND);
#endif
			err = vsfip_httpc_prasehead(httpc, httpc->buf);
			if (err > 0)
			{
				vsfip_buffer_release(httpc->buf);
				continue;
			}
			else if (err < 0)
			{
#ifdef HTTPC_DEBUG
				vsfshell_printf(&httpc->debug_pt, "->ERR HEAD FAIL" VSFSHELL_LINEEND);
#endif
				goto tcp_close;
			}

            if (0 == httpc->resp_length)
			{
				// no data
                goto tcp_close;
			}

			if(httpc->op->on_connect != NULL)
			{
				httpc->local_pt.user_data = httpc->host_mem;
				httpc->local_pt.state = 0;
				vsfsm_pt_entry(pt);
				err = httpc->op->on_connect(&httpc->local_pt, evt);
				if (err > 0) return err; else if (err < 0)
				{
#ifdef HTTPC_DEBUG
					vsfshell_printf(&httpc->debug_pt, "->CONNECT PROCESS FAIL" VSFSHELL_LINEEND);
#endif
					goto tcp_close;
				}
			}

#ifdef HTTPC_DEBUG
			vsfshell_printf(&httpc->debug_pt, "->DATA RECV START" VSFSHELL_LINEEND);
#endif
		}

		if ((httpc->resp_length > 0) && (httpc->buf->app.size > 0))
		{
			if (httpc->op->on_recv != NULL)
			{
				httpc->local_pt.user_data = httpc->host_mem;
				httpc->local_pt.state = 0;
				vsfsm_pt_entry(pt);
				err = httpc->op->on_recv(&httpc->local_pt, evt,
											httpc->resp_curptr, httpc->buf);
				if (err > 0) return err; else if (err < 0)
				{
#ifdef HTTPC_DEBUG
					vsfshell_printf(&httpc->debug_pt, "->DAT RECV PROCESS FAIL" VSFSHELL_LINEEND);
#endif
					goto tcp_close;
				}
			}

			httpc->resp_curptr += httpc->buf->app.size;
			if (httpc->resp_curptr >= httpc->resp_length)
			{
				break;
			}
		}

		vsfip_buffer_release(httpc->buf);
		httpc->buf = NULL;
	}

tcp_close:
		httpc->local_pt.state = 0;
		vsfsm_pt_entry(pt);
		tcp_close_err = vsfip_tcp_close(&httpc->local_pt, evt, httpc->so);
		if (tcp_close_err > 0) return tcp_close_err; else if (tcp_close_err < 0)
		{
#ifdef HTTPC_DEBUG
			vsfshell_printf(&httpc->debug_pt, "->CONNECT FAIL" VSFSHELL_LINEEND);
#endif
			if (!err)
			{
				err = tcp_close_err;
			}
		}

close:
	if (httpc->buf != NULL)
	{
		vsfip_buffer_release(httpc->buf);
	}
	if (httpc->so != NULL)
	{
		vsfip_close(httpc->so);
	}
	vsfsm_pt_end(pt);

	return err;
}

// op_stream
static void vsfip_httpc_outstream_onout_int(void *p)
{
	struct vsfsm_t *sm = (struct vsfsm_t *)p;

	vsfsm_post_evt_pending(sm, VSFSM_EVT_HTTPC_STREAM_OUT);
}

static vsf_err_t
vsfip_httpc_on_connect_stream(struct vsfsm_pt_t *pt, vsfsm_evt_t evt)
{
	struct vsf_stream_t *output = (struct vsf_stream_t *)pt->user_data;

	//config on connect rx
	output->callback_tx.param = pt->sm;
	output->callback_tx.on_out_int = vsfip_httpc_outstream_onout_int;
	stream_connect_tx(output);

	return VSFERR_NONE;
}

static vsf_err_t vsfip_httpc_on_recv_stream(struct vsfsm_pt_t *pt,
				vsfsm_evt_t evt, uint32_t offset, struct vsfip_buffer_t *buf)
{
	struct vsf_stream_t *output = (struct vsf_stream_t *)pt->user_data;

	vsfsm_pt_begin(pt);

	while (stream_get_free_size(output) < buf->app.size)
	{
		vsfsm_pt_wfe(pt, VSFSM_EVT_HTTPC_STREAM_OUT);
	}

	stream_write(output, &buf->app);

	vsfsm_pt_end(pt);
	return VSFERR_NONE;
}

const struct vsfip_httpc_op_t vsfip_httpc_op_stream =
{
	vsfip_httpc_on_connect_stream, vsfip_httpc_on_recv_stream
};

// op_buffer
static vsf_err_t vsfip_httpc_on_recv_buffer(struct vsfsm_pt_t *pt,
				vsfsm_evt_t evt, uint32_t offset, struct vsfip_buffer_t *buf)
{
	struct vsf_buffer_t *output = (struct vsf_buffer_t *)pt->user_data;

	if(offset + buf->app.size < output->size)
	{
		memcpy(output->buffer + offset, buf->app.buffer, buf->app.size);
	}
	else
	{
		memcpy(output->buffer + offset, buf->app.buffer, output->size - offset);
		return VSFERR_FAIL;
	}

	return VSFERR_NONE;
}

const struct vsfip_httpc_op_t vsfip_httpc_op_buffer =
{
	NULL, vsfip_httpc_on_recv_buffer
};
