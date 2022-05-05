#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <sys/ipc.h>
#include <sys/msg.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "config.h"
#include "decoder.h"
#include "mkpath.h"
#include "recpt1.h"
#include "recpt1core.h"

#include "tssplitter_lite.h"

typedef struct _RESOURCE_SET {
	pthread_t *signal_thread;
	pthread_t *reader_thread;
	pthread_t *ipc_thread;
	QUEUE_T **p_queue;
	decoder **decoder;
	splitter **splitter;
	thread_data *tdata;
	sock_data **sockdata;
	int *connected_socket;
	int *listening_socket;
} RESOURCE_SET;

/* maximum write length at once */
#define SIZE_CHANK 1316

/* ipc message size */
#define MSGSZ 255

/* globals */
pthread_mutex_t mutex;
extern boolean f_exit;

//read 1st line from socket
void read_line(int socket, char *p)
{
	int i;
	for (i = 0; i < 255; i++) {
		int ret;
		ret = read(socket, p, 1);
		if (ret == -1) {
			perror("read");
			exit(1);
		} else if (ret == 0) {
			break;
		}
		if (*p == '\n') {
			p++;
			break;
		}
		p++;
	}
	*p = '\0';
}

/* will be ipc message receive thread */
void *mq_recv(void *t)
{
	thread_data *tdata = (thread_data *)t;
	message_buf rbuf;
	char channel[16];
	char service_id[32] = {0};
	int recsec = 0, time_to_add = 0;

	while (1) {
		if (msgrcv(tdata->msqid, &rbuf, MSGSZ, 1, 0) < 0) {
			break;
		}

		sscanf(rbuf.mtext, "ch=%s t=%d e=%d sid=%s", channel, &recsec, &time_to_add, service_id);

		pthread_mutex_lock(&mutex);
		tune(channel, tdata, -1);
		pthread_mutex_unlock(&mutex);

		if (time_to_add) {
			tdata->recsec += time_to_add;
			fprintf(stderr, "Extended %d sec\n", time_to_add);
		}

		if (recsec) {
			time_t cur_time;
			time(&cur_time);
			if (cur_time - tdata->start_time > recsec) {
				f_exit = TRUE;
			} else {
				tdata->recsec = recsec;
				fprintf(stderr, "Total recording time = %d sec\n", recsec);
			}
		}

		if (f_exit)
			break;
	}

	return NULL;
}

QUEUE_T *create_queue(size_t size)
{
	QUEUE_T *p_queue;
	size_t memsize = sizeof(QUEUE_T) + size * sizeof(BUFSZ *);

	p_queue = (QUEUE_T *)malloc(memsize);

	if (p_queue != NULL) {
		p_queue->in = 0;
		p_queue->out = 0;
		p_queue->size = size;
		p_queue->num_avail = size;
		p_queue->num_used = 0;
		pthread_mutex_init(&p_queue->mutex, NULL);
		pthread_cond_init(&p_queue->cond_avail, NULL);
		pthread_cond_init(&p_queue->cond_used, NULL);
	}

	return p_queue;
}

void destroy_queue(QUEUE_T *p_queue)
{
	if (!p_queue)
		return;

	pthread_mutex_destroy(&p_queue->mutex);
	pthread_cond_destroy(&p_queue->cond_avail);
	pthread_cond_destroy(&p_queue->cond_used);
	free(p_queue);
}

/* enqueue data. this function will block if queue is full. */
void enqueue(QUEUE_T *p_queue, BUFSZ *data)
{
	struct timeval now;
	struct timespec spec;
	int retry_count = 0;

	/* enter critical section */
	pthread_mutex_lock(&p_queue->mutex);

	/* wait while queue is full */
	while (p_queue->num_avail == 0) {
		gettimeofday(&now, NULL);
		spec.tv_sec = now.tv_sec + 1;
		spec.tv_nsec = now.tv_usec * 1000;

		pthread_cond_timedwait(&p_queue->cond_avail,
		                       &p_queue->mutex, &spec);
		retry_count++;
		if (retry_count > 60) {
			f_exit = TRUE;
		}
		if (f_exit) {
			pthread_mutex_unlock(&p_queue->mutex);
			return;
		}
	}

	p_queue->buffer[p_queue->in] = data;

	/* move position marker for input to next position */
	p_queue->in++;
	p_queue->in %= p_queue->size;

	/* update counters */
	p_queue->num_avail--;
	p_queue->num_used++;

	/* leave critical section */
	pthread_mutex_unlock(&p_queue->mutex);
	pthread_cond_signal(&p_queue->cond_used);
}

/* dequeue data. this function will block if queue is empty. */
BUFSZ *dequeue(QUEUE_T *p_queue)
{
	struct timeval now;
	struct timespec spec;
	BUFSZ *buffer;
	int retry_count = 0;

	/* enter the critical section*/
	pthread_mutex_lock(&p_queue->mutex);

	/* wait while queue is empty */
	while (p_queue->num_used == 0) {
		gettimeofday(&now, NULL);
		spec.tv_sec = now.tv_sec + 1;
		spec.tv_nsec = now.tv_usec * 1000;

		pthread_cond_timedwait(&p_queue->cond_used,
		                       &p_queue->mutex, &spec);
		retry_count++;
		if (retry_count > 60) {
			f_exit = TRUE;
		}
		if (f_exit) {
			pthread_mutex_unlock(&p_queue->mutex);
			return NULL;
		}
	}

	/* take buffer address */
	buffer = p_queue->buffer[p_queue->out];

	/* move position marker for output to next position */
	p_queue->out++;
	p_queue->out %= p_queue->size;

	/* update counters */
	p_queue->num_avail++;
	p_queue->num_used--;

	/* leave the critical section */
	pthread_mutex_unlock(&p_queue->mutex);
	pthread_cond_signal(&p_queue->cond_avail);

	return buffer;
}

/* this function will be reader thread */
void *reader_func(void *p)
{
	thread_data *tdata = (thread_data *)p;
	QUEUE_T *p_queue = tdata->queue;
	decoder *dec = tdata->decoder;
	splitter *splitter = tdata->splitter;
	int wfd = tdata->wfd;
	boolean use_b25 = dec ? TRUE : FALSE;
	boolean use_udp = tdata->sock_data ? TRUE : FALSE;
	boolean fileless = FALSE;
	boolean use_splitter = splitter ? TRUE : FALSE;
	int sfd = -1;
	pthread_t signal_thread = tdata->signal_thread;
	BUFSZ *qbuf;
	static splitbuf_t splitbuf;
	ARIB_STD_B25_BUFFER dbuf, buf;
	int code;
	int split_select_finish = TSS_ERROR;

	buf.size = 0;
	buf.data = NULL;
	splitbuf.buffer_size = 0;
	splitbuf.buffer = NULL;

	if (wfd == -1)
		fileless = TRUE;

	if (use_udp) {
		sfd = tdata->sock_data->sfd;
	}

	while (1) {
		ssize_t wc = 0;
		int file_err = 0;
		qbuf = dequeue(p_queue);
		/* no entry in the queue */
		if (qbuf == NULL) {
			break;
		}

		buf.data = qbuf->buffer;
		buf.size = qbuf->size;

		if (use_splitter) {
			splitbuf.buffer_filled = 0;

			/* allocate split buffer */
			if (splitbuf.buffer_size < buf.size && buf.size > 0) {
				if (splitbuf.buffer)
					free(splitbuf.buffer);
				splitbuf.buffer = (u_char *)malloc(buf.size);
				if (splitbuf.buffer == NULL) {
					fprintf(stderr, "split buffer allocation failed\n");
					use_splitter = FALSE;
					goto fin;
				}
			}

			while (buf.size) {
				/* 分離対象PIDの抽出 */
				if (split_select_finish != TSS_SUCCESS) {
					split_select_finish = split_select(splitter, &buf);
					if (split_select_finish == TSS_NULL) {
						/* mallocエラー発生 */
						fprintf(stderr, "split_select malloc failed\n");
						use_splitter = FALSE;
						goto fin;
					} else if (split_select_finish != TSS_SUCCESS) {
						/* 分離対象PIDが完全に抽出できるまで出力しない
						 * 1秒程度余裕を見るといいかも
						 */
						time_t cur_time;
						time(&cur_time);
						if (cur_time - tdata->start_time > 4) {
							use_splitter = FALSE;
							goto fin;
						}
						break;
					}
				}

				/* 分離対象以外をふるい落とす */
				code = split_ts(splitter, &buf, &splitbuf);
				if (code == TSS_NULL) {
					fprintf(stderr, "PMT reading..\n");
				} else if (code != TSS_SUCCESS) {
					fprintf(stderr, "split_ts failed\n");
					break;
				}

				break;
			} /* while */

			buf.data = splitbuf.buffer;
			buf.size = splitbuf.buffer_filled;

		fin:;
		} /* if */

		if (use_b25) {
			int lp = 0;

			while (1) {
				code = b25_decode(dec, &buf, &dbuf);
				if (code < 0) {
					fprintf(stderr, "b25_decode failed (code=%d).", code);
					if (lp++ < 5) {
						/* restart b25 decoder */
						fprintf(stderr, "\ndecoder restart! \n");
						b25_shutdown(dec);
						b25_startup(tdata->dopt);
					} else {
						fprintf(stderr, " fall back to encrypted recording.\n");
						use_b25 = FALSE;
						break;
					}
				} else {
					buf = dbuf;
					break;
				}
			}
		}

		if (!fileless) {
			/* write data to output file */
			int size_remain = buf.size;
			int offset = 0;

			while (size_remain > 0) {
				int ws = size_remain < SIZE_CHANK ? size_remain : SIZE_CHANK;

				wc = write(wfd, buf.data + offset, ws);
				if (wc < 0) {
					perror("write");
					file_err = 1;
					pthread_kill(signal_thread,
					             errno == EPIPE ? SIGPIPE : SIGUSR2);
					break;
				}
				size_remain -= wc;
				offset += wc;
			}
		}

		if (use_udp && sfd != -1) {
			/* write data to socket */
			int size_remain = buf.size;
			int offset = 0;
			while (size_remain > 0) {
				int ws = size_remain < SIZE_CHANK ? size_remain : SIZE_CHANK;
				wc = write(sfd, buf.data + offset, ws);
				if (wc < 0) {
					if (errno == EPIPE)
						pthread_kill(signal_thread, SIGPIPE);
					break;
				}
				size_remain -= wc;
				offset += wc;
			}
		}

		free(qbuf);
		qbuf = NULL;

		/* normal exit */
		if ((f_exit && !p_queue->num_used) || file_err) {
			if (use_b25) {
				code = b25_finish(dec, &dbuf);
				if (code < 0)
					fprintf(stderr, "b25_finish failed\n");
				else
					buf = dbuf;
			}

			if (!fileless && !file_err) {
				wc = write(wfd, buf.data, buf.size);
				if (wc < 0) {
					perror("write");
					file_err = 1;
					pthread_kill(signal_thread,
					             errno == EPIPE ? SIGPIPE : SIGUSR2);
				}
			}

			if (use_udp && sfd != -1) {
				wc = write(sfd, buf.data, buf.size);
				if (wc < 0) {
					if (errno == EPIPE)
						pthread_kill(signal_thread, SIGPIPE);
				}
			}

			break;
		}
	}

	if (splitbuf.buffer) {
		free(splitbuf.buffer);
		splitbuf.buffer = NULL;
		splitbuf.buffer_size = 0;
	}

	time_t cur_time;
	time(&cur_time);
	fprintf(stderr, "Recorded %dsec\n",
			(int)(cur_time - tdata->start_time));

	return NULL;
}

void show_usage(char *cmd)
{
	fprintf(stderr, "%s Ver %s\n", cmd, version);
#ifdef HAVE_LIBARIB25
	fprintf(stderr, "Usage: \n%s [--b25 [--strip] [--emm] [--round N]] [--udp [--addr hostname --port portnumber]] [--http portnumber] [--dev devicenumber] [--lnb voltage] [--sid SID1,SID2] channel rectime destfile\n", cmd);
#else
	fprintf(stderr, "Usage: \n%s [--udp [--addr hostname --port portnumber]] [--http portnumber] [--dev devicenumber] [--lnb voltage] [--sid SID1,SID2] channel rectime destfile\n", cmd);
#endif
	fprintf(stderr, "\n");
	fprintf(stderr, "Remarks:\n");
	fprintf(stderr, "if rectime  is '-', records indefinitely.\n");
	fprintf(stderr, "if destfile is '-', stdout is used for output.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "LNB control: %s --dev devicenumber --lnb voltage\n", cmd);
}

void show_options(void)
{
	fprintf(stderr, "Options:\n");
#ifdef HAVE_LIBARIB25
	fprintf(stderr, "--b25:               Decrypt using BCAS card\n");
	fprintf(stderr, "  --strip:           Strip null stream\n");
	fprintf(stderr, "  --emm:             Instruct EMM operation\n");
	fprintf(stderr, "  --round N:         Specify round number\n");
#endif
	fprintf(stderr, "--udp:               Turn on udp broadcasting\n");
	fprintf(stderr, "  --addr hostname:   Hostname or address to connect\n");
	fprintf(stderr, "  --port portnumber: Port number to connect\n");
	fprintf(stderr, "--http portnumber:   Turn on http broadcasting (run as a daemon)\n");
	fprintf(stderr, "--dev N:             Use DVB device /dev/dvb/adapterN\n");
	fprintf(stderr, "--lnb voltage:       Specify LNB voltage (0, 11, 15)\n");
	fprintf(stderr, "--sid SID1,SID2,...: Specify SID number in CSV format (101,102,...)\n");
	fprintf(stderr, "--help:              Show this help\n");
	fprintf(stderr, "--version:           Show version\n");
	fprintf(stderr, "--list:              Show channel list\n");
}

void cleanup(thread_data *tdata)
{
	/* stop recording */
	f_exit = TRUE;

	pthread_cond_signal(&tdata->queue->cond_avail);
	pthread_cond_signal(&tdata->queue->cond_used);
}

/* will be signal handler thread */
void *process_signals(void *t)
{
	sigset_t waitset;
	int sig;
	thread_data *tdata = (thread_data *)t;

	sigemptyset(&waitset);
	sigaddset(&waitset, SIGPIPE);
	sigaddset(&waitset, SIGINT);
	sigaddset(&waitset, SIGTERM);
	sigaddset(&waitset, SIGUSR1);
	sigaddset(&waitset, SIGUSR2);

	sigwait(&waitset, &sig);

	switch (sig) {
		case SIGPIPE:
			fprintf(stderr, "\nSIGPIPE received. cleaning up...\n");
			cleanup(tdata);
			break;
		case SIGINT:
			fprintf(stderr, "\nSIGINT received. cleaning up...\n");
			cleanup(tdata);
			break;
		case SIGTERM:
			fprintf(stderr, "\nSIGTERM received. cleaning up...\n");
			cleanup(tdata);
			break;
		case SIGUSR1: /* normal exit*/
			cleanup(tdata);
			break;
		case SIGUSR2: /* error */
			fprintf(stderr, "Detected an error. cleaning up...\n");
			cleanup(tdata);
			break;
	}

	return NULL; /* dummy */
}

void init_signal_handlers(pthread_t *signal_thread, thread_data *tdata)
{
	sigset_t blockset;

	sigemptyset(&blockset);
	sigaddset(&blockset, SIGPIPE);
	sigaddset(&blockset, SIGINT);
	sigaddset(&blockset, SIGTERM);
	sigaddset(&blockset, SIGUSR1);
	sigaddset(&blockset, SIGUSR2);

	if (pthread_sigmask(SIG_BLOCK, &blockset, NULL))
		fprintf(stderr, "pthread_sigmask() failed.\n");

	pthread_create(signal_thread, NULL, process_signals, tdata);
}

int set_ch_table(void)
{
	FILE *fp;
	char *p, buf[256];
	ssize_t len;

	if ((len = readlink("/proc/self/exe", buf, sizeof(buf) - 8)) == -1)
		return 2;
	buf[len] = '\0';
	strcat(buf, ".conf");

	fp = fopen(buf, "r");
	if (fp == NULL) {
		fprintf(stderr, "Cannot open '%s'\n", buf);
		return 1;
	}

	int i = 0;
	len = sizeof(isdb_conv_table[0].channel) - 1;
	while (fgets(buf, sizeof(buf), fp) && i < MAX_CH - 1) {
		if (buf[0] == ';')
			continue;
		p = buf + strlen(buf) - 1;
		while ((p >= buf) && (*p == '\r' || *p == '\n'))
			*p-- = '\0';
		if (p < buf)
			continue;

		int n = 0;
		char *cp[3];
		boolean bOk = FALSE;
		p = cp[n++] = buf;
		while (1) {
			p = strchr(p, '\t');
			if (p) {
				*p++ = '\0';
				cp[n++] = p;
				if (n > 2) {
					bOk = TRUE;
					break;
				}
			} else
				break;
		}
		if (bOk) {
			strncpy(isdb_conv_table[i].channel, cp[0], len);
			isdb_conv_table[i].channel[len] = '\0';
			modify_ch_str(isdb_conv_table[i].channel);
			isdb_conv_table[i].freq_no = (int)strtol(cp[1], NULL, 10);
			isdb_conv_table[i].tsid = (unsigned int)strtol(cp[2], NULL, 0);
			i++;
		}
	}

	fclose(fp);
	isdb_conv_table[i].channel[0] = '\0';
	isdb_conv_table[i].tsid = 0;

	return 0;
}

int release_resources(RESOURCE_SET *res)
{
	int ret = 0;

	if (*res->sockdata) {
		if ((*res->sockdata)->sfd > -1)
			close((*res->sockdata)->sfd);
		free(*res->sockdata);
	}

	if (*res->splitter)
		split_shutdown(*res->splitter);

	if (res->tdata->wfd > 2) {
		fsync(res->tdata->wfd);
		close(res->tdata->wfd);
	}

	if (*res->decoder)
		b25_shutdown(*res->decoder);

	if (res->tdata)
		ret = close_tuner(res->tdata);

	if (*res->listening_socket > 2)
		close(*res->listening_socket);

	return ret;
}

int main(int argc, char **argv)
{
	time_t cur_time;
	pthread_t signal_thread;
	pthread_t reader_thread;
	pthread_t ipc_thread;
	BUFSZ *bufptr;
	decoder *decoder = NULL;
	splitter *splitter = NULL;
	static thread_data tdata;
	decoder_options dopt = {
		4, /* round */
		0, /* strip */
		0  /* emm */
	};
	tdata.dopt = &dopt;
	tdata.lnb = -1;
	tdata.tfd = -1;
	tdata.fefd = 0;
	tdata.dmxfd = 0;
	tdata.wfd = -1;
	tdata.table = NULL;

	int result;
	int option_index;
	struct option long_options[] = {
#ifdef HAVE_LIBARIB25
		{"b25",     0, NULL, 'b'},
		{"B25",     0, NULL, 'b'},
		{"round",   1, NULL, 'r'},
		{"strip",   0, NULL, 's'},
		{"emm",     0, NULL, 'm'},
		{"EMM",     0, NULL, 'm'},
#endif
		{"LNB",     1, NULL, 'n'},
		{"lnb",     1, NULL, 'n'},
		{"udp",     0, NULL, 'u'},
		{"addr",    1, NULL, 'a'},
		{"port",    1, NULL, 'p'},
		{"http",    1, NULL, 'H'},
		{"dev",     1, NULL, 'd'},
		{"help",    0, NULL, 'h'},
		{"version", 0, NULL, 'v'},
		{"list",    0, NULL, 'l'},
		{"sid",     1, NULL, 'i'},
		{0, 0, NULL, 0} /* terminate */
	};

	boolean use_b25 = FALSE;
	boolean use_udp = FALSE;
	boolean use_http = FALSE;
	boolean fileless = FALSE;
	boolean use_splitter = FALSE;
	char *host_to = NULL;
	int port_to = 1234;
	int port_http = 12345;
	sock_data *sockdata = NULL;
	int dev_num = -1;
	int val;
	char *voltage[] = {"11V", "15V", "0V"};
	char *sid_list = NULL;
	int connected_socket = -1;
	int listening_socket = -1;
	unsigned int len;
	char *channel = NULL;
	RESOURCE_SET resource_set;

	if (set_ch_table() != 0)
		return 1;

	resource_set.decoder = &decoder;
	resource_set.splitter = &splitter;
	resource_set.tdata = &tdata;
	resource_set.sockdata = &sockdata;
	resource_set.listening_socket = &listening_socket;

	while ((result = getopt_long(argc, argv, "br:smn:ua:H:p:d:hvli:",
								 long_options, &option_index)) != -1) {
		switch (result) {
			case 'b':
				use_b25 = TRUE;
				fprintf(stderr, "using B25...\n");
				break;
			case 's':
				dopt.strip = TRUE;
				fprintf(stderr, "enable B25 strip\n");
				break;
			case 'm':
				dopt.emm = TRUE;
				fprintf(stderr, "enable B25 emm processing\n");
				break;
			case 'u':
				use_udp = TRUE;
				host_to = "localhost";
				fprintf(stderr, "enable UDP broadcasting\n");
				break;
			case 'H':
				use_http = TRUE;
				port_http = (int)strtol(optarg, NULL, 10);
				fprintf(stderr, "creating a http daemon\n");
				break;
			case 'h':
				show_usage(argv[0]);
				fprintf(stderr, "\n");
				show_options();
				fprintf(stderr, "\n");
				show_channels();
				fprintf(stderr, "\n");
				exit(0);
				break;
			case 'v':
				fprintf(stderr, "%s %s\n", argv[0], version);
				fprintf(stderr, "recorder command for DVB tuner.\n");
				exit(0);
				break;
			case 'l':
				show_channels();
				exit(0);
				break;
			/* following options require argument */
			case 'n':
				val = (int)strtol(optarg, NULL, 10);
				switch (val) {
					case 11:
						tdata.lnb = 0;  // SEC_VOLTAGE_13 日本は11V(PT1/2/3は12V)
						break;
					case 15:
						tdata.lnb = 1;  // SEC_VOLTAGE_18 日本は15V
						break;
					default:
						tdata.lnb = 2;  // SEC_VOLTAGE_OFF
						break;
				}
				fprintf(stderr, "LNB = %s\n", voltage[tdata.lnb]);
				break;
			case 'r':
				dopt.round = (int)strtol(optarg, NULL, 10);
				fprintf(stderr, "set round %d\n", dopt.round);
				break;
			case 'a':
				use_udp = TRUE;
				host_to = optarg;
				fprintf(stderr, "UDP destination address: %s\n", host_to);
				break;
			case 'p':
				port_to = (int)strtol(optarg, NULL, 10);
				fprintf(stderr, "UDP port: %d\n", port_to);
				break;
			case 'd':
				dev_num = (int)strtol(optarg, NULL, 10);
				fprintf(stderr, "using device: /dev/dvb/adapter%d", dev_num);
				break;
			case 'i':
				use_splitter = TRUE;
				sid_list = optarg;
				break;
		}
	}

	if (use_http) {  // http-server
		fprintf(stderr, "run as a daemon..\n");
		if (daemon(1, 1)) {
			perror("failed to start");
			return 1;
		}
		fprintf(stderr, "pid = %d\n", getpid());

		struct sockaddr_in sin;
		int ret;
		int sock_optval = 1;

		listening_socket = socket(AF_INET, SOCK_STREAM, 0);
		if (listening_socket == -1) {
			perror("socket");
			return 1;
		}

		if (setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR,
		               &sock_optval, sizeof(sock_optval)) == -1) {
			perror("setsockopt");
			close(listening_socket);
			return 1;
		}

		sin.sin_family = AF_INET;
		sin.sin_port = htons(port_http);
		sin.sin_addr.s_addr = htonl(INADDR_ANY);

		if (bind(listening_socket, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
			perror("bind");
			close(listening_socket);
			return 1;
		}

		ret = listen(listening_socket, SOMAXCONN);
		if (ret == -1) {
			perror("listen");
			close(listening_socket);
			return 1;
		}
		fprintf(stderr, "listening at port %d\n", port_http);
		/* set rectime to the infinite */
		if (parse_time("-", &tdata.recsec) != 0) {
			close(listening_socket);
			return 1;
		}
		if (tdata.recsec == -1)
			tdata.indefinite = TRUE;
	} else {  // http-server
		if (argc - optind < 3) {
			if (argc - optind == 2 && use_udp) {
				fprintf(stderr, "Fileless UDP broadcasting\n");
				fileless = TRUE;
			} else {
				if (argc == optind && dev_num != -1 && tdata.lnb != -1) {
					return lnb_control(dev_num, tdata.lnb);
				} else {
					show_usage(argv[0]);
					fprintf(stderr, "\n");
					fprintf(stderr, "Arguments are necessary!\n");
					fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
					return 1;
				}
			}
		}

		fprintf(stderr, "pid = %d\n", getpid());

		/* set recsec */
		if (parse_time(argv[optind + 1], &tdata.recsec) != 0)  // no other thread --yaz
			return 1;

		/* tune */
		if (tune(argv[optind], &tdata, dev_num) != 0)
			return 1;

		if (tdata.recsec == -1)
			tdata.indefinite = TRUE;

		/* open output file */
		char *destfile = argv[optind + 2];
		if (destfile && !strcmp("-", destfile)) {
			tdata.wfd = 1; // stdout
		} else {
			if (!fileless) {
				int status;
				char *path = strdup(argv[optind + 2]);
				char *dir = dirname(path);
				status = mkpath(dir, 0777);
				if (status == -1)
					perror("mkpath");
				free(path);

				tdata.wfd = open(argv[optind + 2], (O_RDWR | O_CREAT | O_TRUNC), 0666);
				if (tdata.wfd < 0) {
					fprintf(stderr, "Cannot open output file: %s\n",
							argv[optind + 2]);
					close_tuner(&tdata);
					return 1;
				}
			}
		}
	}  // http-server

	/* initialize decoder */
	if (use_b25) {
		decoder = b25_startup(&dopt);
		if (!decoder) {
			fprintf(stderr, "Cannot start b25 decoder\n");
			fprintf(stderr, "Fall back to encrypted recording\n");
			use_b25 = FALSE;
		}
	}

	while (1) {  // http-server
		result = 1;
		if (use_http) {
			struct hostent *peer_host;
			struct sockaddr_in peer_sin;

			len = sizeof(peer_sin);

			connected_socket = accept(listening_socket, (struct sockaddr *)&peer_sin, &len);
			if (connected_socket == -1) {
				perror("accept");
				break;
			}

			peer_host = gethostbyaddr((char *)&peer_sin.sin_addr.s_addr,
			                          sizeof(peer_sin.sin_addr), AF_INET);
			char *h_name;
			if (peer_host == NULL) {
				h_name = "NONAME";
			} else
				h_name = peer_host->h_name;

			fprintf(stderr, "connect from: %s [%s] port %d\n", h_name,
					inet_ntoa(peer_sin.sin_addr), ntohs(peer_sin.sin_port));

			char buf[256];
			read_line(connected_socket, buf);
			fprintf(stderr, "request command is %s\n", buf);
			char s0[256], s1[256], s2[256];
			sscanf(buf, "%s%s%s", s0, s1, s2);
			char delim[] = "/";
			channel = strtok(s1, delim);
			if (!strcmp(channel, "0")) {
				result = 0;
				break;
			}
			char *sidflg = strtok(NULL, delim);
			if (sidflg)
				sid_list = sidflg;
			fprintf(stderr, "channel is %s\n", channel);

			if (sid_list == NULL) {
				use_splitter = FALSE;
				splitter = NULL;
			} else if (!strcmp(sid_list, "all")) {
				use_splitter = FALSE;
				splitter = NULL;
			} else {
				use_splitter = TRUE;
			}
		}  // http-server

		/* initialize splitter */
		if (use_splitter) {
			splitter = split_startup(sid_list);
			if (splitter->sid_list == NULL) {
				fprintf(stderr, "Cannot start TS splitter\n");
				break;
			}
		}

		if (use_http) {  // http-server
			char header[] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-cache\r\n\r\n";
			int len = strlen(header);

			/* set write target to http */
			tdata.wfd = connected_socket;

			/* tune */
			result = tune(channel, &tdata, -1);
			if (!result)
				result = write(tdata.wfd, header, len) < len;
			if (result) {
				close(tdata.wfd);
				tdata.wfd = -1;
				if (use_splitter) {
					split_shutdown(splitter);
					splitter = NULL;
				}
				continue;
			}
		} else {  // http-server
			/* initialize udp connection */
			if (use_udp) {
				sockdata = (sock_data *)malloc(sizeof(sock_data));
				sockdata->sfd = -1;
				struct in_addr ia;
				ia.s_addr = inet_addr(host_to);
				if (ia.s_addr == INADDR_NONE) {
					struct hostent *hoste = gethostbyname(host_to);
					if (!hoste) {
						perror("gethostbyname");
						break;
					}
					ia.s_addr = *(in_addr_t *)(hoste->h_addr_list[0]);
				}
				if ((sockdata->sfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
					perror("socket");
					break;
				}

				sockdata->addr.sin_family = AF_INET;
				sockdata->addr.sin_port = htons(port_to);
				sockdata->addr.sin_addr.s_addr = ia.s_addr;

				if (connect(sockdata->sfd, (struct sockaddr *)&sockdata->addr,
							sizeof(sockdata->addr)) < 0) {
					perror("connect");
					break;
				}
			}
		}  // http-server

		/* initialize mutex */
		pthread_mutex_init(&mutex, NULL);

		/* prepare thread data */
		tdata.queue = create_queue(MAX_QUEUE);
		tdata.decoder = decoder;
		tdata.splitter = splitter;
		tdata.sock_data = sockdata;
		tdata.tune_persistent = FALSE;

		/* spawn signal handler thread */
		init_signal_handlers(&signal_thread, &tdata);

		/* spawn reader thread */
		tdata.signal_thread = signal_thread;
		pthread_create(&reader_thread, NULL, reader_func, &tdata);

		/* spawn ipc thread */
		key_t key;
		key = (key_t)getpid();

		if ((tdata.msqid = msgget(key, IPC_CREAT | 0666)) < 0) {
			perror("msgget");
		}
		pthread_create(&ipc_thread, NULL, mq_recv, &tdata);
		if (use_http || use_udp)
			fprintf(stderr, "\nSending...\n");
		else
			fprintf(stderr, "\nRecording...\n");

		time(&tdata.start_time);

		/* read from tuner */
		while (1) {
			if (f_exit)
				break;

			time(&cur_time);
			bufptr = malloc(sizeof(BUFSZ));
			if (!bufptr) {
				f_exit = TRUE;
				break;
			}
			pthread_mutex_lock(&mutex);
			bufptr->size = read(tdata.tfd, bufptr->buffer, MAX_READ_SIZE);
			pthread_mutex_unlock(&mutex);
			if (bufptr->size <= 0) {
				if ((cur_time - tdata.start_time) >= tdata.recsec && !tdata.indefinite) {
					f_exit = TRUE;
					enqueue(tdata.queue, NULL);
					break;
				} else {
					free(bufptr);
					continue;
				}
			}
			enqueue(tdata.queue, bufptr);

			/* stop recording */
			if ((cur_time - tdata.start_time) >= tdata.recsec && !tdata.indefinite) {
				// ここは何とかしたいところ
				/* read remaining data */
				do {
					bufptr = malloc(sizeof(BUFSZ));
					if (!bufptr) {
						f_exit = TRUE;
						break;
					}
					pthread_mutex_lock(&mutex);
					bufptr->size = read(tdata.tfd, bufptr->buffer, MAX_READ_SIZE);
					pthread_mutex_unlock(&mutex);
					if (bufptr->size <= 0) {
						f_exit = TRUE;
						enqueue(tdata.queue, NULL);
						break;
					}
					enqueue(tdata.queue, bufptr);
				} while (cur_time == time(NULL));
				break;
			}
		}

		/* delete message queue*/
		msgctl(tdata.msqid, IPC_RMID, NULL);

		pthread_kill(signal_thread, SIGUSR1);

		/* wait for threads */
		pthread_join(reader_thread, NULL);
		pthread_join(signal_thread, NULL);
		pthread_join(ipc_thread, NULL);

		/* destroy mutex */
		pthread_mutex_destroy(&mutex);

		/* release queue */
		destroy_queue(tdata.queue);
		tdata.queue = NULL;

		if (use_http) {  // http-server
			/* close http socket */
			close(tdata.wfd);
			tdata.wfd = -1;

			if (use_splitter) {
				split_shutdown(splitter);
				splitter = NULL;
			}

			fprintf(stderr, "connection closed. still listening at port %d\n", port_http);
			f_exit = FALSE;
		} else {  // http-server
			result = 0;
			break;
		}
	}  // http-server

	result |= release_resources(&resource_set);
	return result;
}
