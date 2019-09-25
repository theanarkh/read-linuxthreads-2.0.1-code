/*
 * Simple WEB Proxy for Linux and perhaps other systems
 *     by Willy Tarreau
 *
 *             This program is used to redirect HTTP requests
 *             it receives to the right HTTP server, or to
 *             another instance of itself, on an other host.
 *             It acts like a proxy and all the Web browsers
 *             that will have to use it must be setup to use
 *             it as the HTTP Proxy. It then allows several
 *             hosts on a network to access the Web via one
 *             only server, which is particularly interesting
 *             in case of a server connected to an Internet
 *             provider via a modem with PPP.
 *
 *             One interesting aspect is that it doesn't require
 *             superuser privileges to run  :-)
 *
 * Authors:    based on stuff by
 *                 Willy Tarreau <tarreau@aemiaif.ibp.fr>
 * 
 *             Multithreaded code, POST http method, SIGPIPE, fixes, ... 
 * 		   (rework)
 *                 Pavel Krauz <kra@fsid.cvut.cz>
 * 
 * 
 * Todo:       - Make a list of hosts and network which can be
 *               accessed directly, and those which need another
 *               proxy.
 *             - add an option to supply an access log with
 *               hostnames and requests.
 *
 *  Copyright (C) 1996  <Willy Tarreau>
 *  E-mail: tarreau@aemiaif.ibp.fr
 * 
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <pthread.h>

/* default listen port */
#define LISTENPORT  8080

/* 
 * default timeout for any read or write, in seconds 
 */
#define TIMEOUT_OUT 		60

/* 
 * exit timeout for idle thread
 */
#define TIMEOUT_THREAD_EXIT	15

/* length of data buffer in chars */
#define LDATA       1024

/* length of remote server address */
#define LADR        128

/* default port to connect to if unspecified */
#define DEFAULTPORT 80

/*
 * max proxy threads for requests
 */
#if 1
#define MAX_PROXY_THREADS	64
#else
#define MAX_PROXY_THREADS	4
#endif

int ConnectToProxy = 0;		/* 1 here means this program will connect to another instance of it
				   0 means we'll connect directly to the Internet */
char NextProxyAdr[128];		/* the name of the host where the next instance of the program runs */
int NextProxyPort;		/* and its port */
int NoCache = 0;		/* if not 0, prevents web browsers from retrieving pages in their own
				   cache when the users does a "Reload" action */
int timeout_out = TIMEOUT_OUT;
int max_proxy_threads = MAX_PROXY_THREADS;

struct th_proxy_struct {
	pthread_t th;		/* only for server */

	struct th_proxy_struct *next_free;
	pthread_mutex_t mu;
	pthread_cond_t cond;
	int sock_in;
};

pthread_mutex_t free_q_mu;
pthread_cond_t free_q_cond;
struct th_proxy_struct *free_q;
int thread_count = 0;	/* protected with free_q_mu */
pthread_key_t key_alarm;

pthread_mutex_t gethostbyname_mu;	/* used for protect gethostbyname 
					 * for gethostbyname_r it isn't needed
					 */

void request_alarm(int n);
void server(int sockListen, struct sockaddr_in *inputSocket);
void *client(struct th_proxy_struct *th_proxy);
void displaysyntax(void);

char *BADREQ =
"HTTP/1.0 400 ERROR\r\n"
"Server: thproxyd\r\n"
"Content-type: text/html\r\n"
"\r\n"
"<HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n"
"<BODY><H1>400 Bad Request</H1>\n"
"Your client sent a query that this server could not\n"
"understand.<P>\n"
"Reason: Invalid or unsupported method.<P>\n"
"</BODY>\n";

char *SERVERR =
"HTTP/1.0 500 ERROR\r\n"
"Server: thproxyd\r\n"
"Content-type: text/html\r\n"
"\r\n"
"<HEAD><TITLE>500 Server Error</TITLE></HEAD>\n"
"<BODY><H1>500 Server Error</H1>\n"
"Internal proxy error while processing your query.<P>\n"
"Reason: Internal proxy error.<P>\n"
"</BODY>\n";

char *SERVCOERR =
"HTTP/1.0 500 ERROR\r\n"
"Server: thproxyd\r\n"
"Content-type: text/html\r\n"
"\r\n"
"<HEAD><TITLE>500 Server Error</TITLE></HEAD>\n"
"<BODY><H1>500 Server Error</H1>\n"
"Internal proxy error while processing your query.<P>\n"
"Reason: Invalid connection.<P>\n"
"</BODY>\n";

char *SERVDNSERR =
"HTTP/1.0 500 ERROR\r\n"
"Server: thproxyd\r\n"
"Content-type: text/html\r\n"
"\r\n"
"<HEAD><TITLE>500 Server Error</TITLE></HEAD>\n"
"<BODY><H1>500 Server Error</H1>\n"
"Internal proxy error while processing your query.<P>\n"
"Reason: Bad address - DNS cann't resolve address.<P>\n"
"</BODY>\n";

char *SERVTIMEOUT =
"HTTP/1.0 500 ERROR\r\n"
"Server: thproxyd\r\n"
"Content-type: text/html\r\n"
"\r\n"
"<HEAD><TITLE>500 Server Error</TITLE></HEAD>\n"
"<BODY><H1>500 Server Error</H1>\n"
"Internal proxy error while processing your query.<P>\n"
"Reason: Server time out while connection establishment or data transfer.<P>\n"
"</BODY>\n";

char *POSTERR =
"HTTP/1.0 500 ERROR\r\n"
"Server: thproxyd\r\n"
"Content-type: text/html\r\n"
"\r\n"
"<HEAD><TITLE>500 Proxy Server Error</TITLE></HEAD>\n"
"<BODY><H1>500 Proxy Server Error</H1>\n"
"Failed to POST.<P>\n"
"Reason: post method error ???.<P>\n"
"</BODY>\n";


void main(int argc, char **argv)
{
	int listenport = LISTENPORT;
	struct sockaddr_in ListenSocket;
	int sockListen;
	struct sigaction sa;
	int opt, val;

	while ((opt = getopt(argc, argv, "p:x:t:nm:")) != -1) {
		switch (opt) {
		case ':':	/* missing parameter */
		case '?':	/* unknown option */
			displaysyntax();
			exit(1);

		case 'p':	/* port */
			listenport = atoi(optarg);
			break;

		case 'x':{	/* external proxy */
				char *p;
				p = strchr(optarg, ':');
				if (p == NULL) {	/* unspecified port number. let's quit */
					fprintf(stderr, "missing port for next proxy\n");
					displaysyntax();
					exit(1);
				}
				*(p++) = 0;	/* ends hostname */
				NextProxyPort = atoi(p);
				strcpy(NextProxyAdr, optarg);
				ConnectToProxy = 1;
				break;
			}

		case 't':	/* disconnect time-out */
			timeout_out = atoi(optarg);
			break;

		case 'n':	/* no cache */
			NoCache = 1;
			break;
		case 'm':
			max_proxy_threads = atoi(optarg);
			break;
		}		/* end of switch() */
	}			/* end of while() */

	/* initialization of listen socket */

	pthread_mutex_init(&free_q_mu, NULL);
	pthread_mutex_init(&gethostbyname_mu, NULL);
	pthread_cond_init(&free_q_cond, NULL);
	free_q = NULL;

	pthread_key_create(&key_alarm, NULL);

	sa.sa_handler = request_alarm;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGALRM, &sa, NULL);

	if ((sockListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1) {
		perror("Webroute(socket)");
		exit(1);
	}
	val = 1;
	if ((setsockopt(sockListen, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val))) == -1) {
		perror("Webroute(setsockopt)");
		exit(1);
	}
	bzero((char *) &ListenSocket, sizeof(ListenSocket));
	ListenSocket.sin_family = AF_INET;
	ListenSocket.sin_addr.s_addr = htonl(INADDR_ANY);
	ListenSocket.sin_port = htons(listenport);
	if (bind(sockListen, (struct sockaddr *) &ListenSocket, sizeof(ListenSocket)) == -1) {
		perror("Webroute(bind)");
		exit(1);
	}
	if (listen(sockListen, 5) == -1) {
		perror("Webroute(listen)");
		exit(1);
	}
	/* the socket is ready. Let's wait for requests ... */
	/* let's close stdin, stdout, stderr to prevent messages from appearing on the console */
#ifndef DEBUG
	close(0);
	close(1);
	close(2);
#endif
	server(sockListen, &ListenSocket);
}				/* end of main() */

static struct th_proxy_struct *new_proxy_th(void)
{
	struct th_proxy_struct *th_proxy;
	pthread_attr_t attr;

	if (!(th_proxy = malloc(sizeof(struct th_proxy_struct))))
		 return NULL;
	memset(th_proxy, 0, sizeof(struct th_proxy_struct));
	pthread_mutex_init(&th_proxy->mu, NULL);
	pthread_cond_init(&th_proxy->cond, NULL);
	th_proxy->next_free = NULL;
	th_proxy->sock_in = -1;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th_proxy->th, &attr, (void *((*)(void *))) client, th_proxy) != 0) {
		free(th_proxy);
		return NULL;
	}
	return th_proxy;
}

static struct th_proxy_struct *alloc_proxy_th(void)
{
	struct th_proxy_struct *th_proxy;

	pthread_mutex_lock(&free_q_mu);
	do {
		th_proxy = free_q;
		if (free_q)
			free_q = free_q->next_free;
		else {
			if (thread_count < max_proxy_threads) {
				if ((th_proxy = new_proxy_th()))
					thread_count++;
			}
			if (!th_proxy)
				pthread_cond_wait(&free_q_cond, &free_q_mu);
		}
	} while (!th_proxy);
	pthread_mutex_unlock(&free_q_mu);
	return th_proxy;
}

void server(int sockListen, struct sockaddr_in *inputSocket)
{
	int lgInputSocket;
	int sockIn;
	struct th_proxy_struct *th_proxy;

	for (;;) {		/* infinite loop */
		lgInputSocket = sizeof(*inputSocket);
		do {
			sockIn = accept(sockListen, (struct sockaddr *) inputSocket, &lgInputSocket);
		} while (sockIn == -1 && errno == EINTR);	/* retries if interrupted */
		if (sockIn == -1) {	/* if there's an error, we exit */
			exit(1);	/* don't wait when daemon is going sick ! */
		}
		/* process request, alloc thread for it */
		th_proxy = alloc_proxy_th();
		pthread_mutex_lock(&th_proxy->mu);
		th_proxy->sock_in = sockIn;
		pthread_mutex_unlock(&th_proxy->mu);
		pthread_cond_signal(&th_proxy->cond);
	}
}

#define ERR_SOCKET		-5
#define ERR_GETHOSTBYNAME	-6
#define ERR_CONNECT		-7
#define ERR_CONNECT_TIMEOUT	-8

int connectto(char *hote, int port, int sockIn)
{
#if 0	/*
	 * use gethostbyname 
	 */
	struct hostent *ServerName;
	struct sockaddr_in OutputSocket;	/* output socket descriptor */
	int sockOut, retval;

	if ((sockOut = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		return ERR_SOCKET;
	}
	pthread_mutex_lock(&gethostbyname_mu);
	if ((ServerName = gethostbyname(hote)) == NULL) {
		pthread_mutex_unlock(&gethostbyname_mu);
		return ERR_GETHOSTBYNAME;
	}
	bzero((char *) &OutputSocket, sizeof(OutputSocket));
	OutputSocket.sin_family = AF_INET;
	OutputSocket.sin_port = htons(port);
	bcopy((char *) ServerName->h_addr,
	      (char *) &OutputSocket.sin_addr,
	      ServerName->h_length);
	pthread_mutex_unlock(&gethostbyname_mu);
#else
	/*
	 * use gethostbyname_r 
	 */
	struct hostent ServerName;
	struct sockaddr_in OutputSocket;	/* output socket descriptor */
	int sockOut, retval;
	char buf[1024];
	int hst_errno;

	if ((sockOut = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		return ERR_SOCKET;
	}
	/*
	 * pointers used in struct hostent ServerName will be point to space
	 * which user give to function (the buf variable)
	 */
	if (gethostbyname_r(hote, &ServerName, buf, sizeof(buf), &hst_errno)
	    == NULL) {
		if (hst_errno == ERANGE) {
			/* the buf is to short for store all informations */
		}
		return ERR_GETHOSTBYNAME;
	}
	bzero((char *) &OutputSocket, sizeof(OutputSocket));
	OutputSocket.sin_family = AF_INET;
	OutputSocket.sin_port = htons(port);
	bcopy((char *) ServerName.h_addr,
	      (char *) &OutputSocket.sin_addr,
	      ServerName.h_length);
#endif
	alarm(timeout_out);
	retval = connect(sockOut, (struct sockaddr *) &OutputSocket, sizeof(OutputSocket));
	alarm(0);
	if (retval == -1) {
		close(sockOut);
		return ERR_CONNECT;
	}
	return sockOut;		/* connection OK */
}

/* this function should be called only by the child */
void sayerror(char *msg, int sockIn, int sockOut)
{
	struct linger linger;

	pthread_setspecific(key_alarm, NULL);
	
	write(sockIn, msg, strlen(msg));
	
	linger.l_onoff = 1;
	linger.l_linger = 4;
	setsockopt(sockIn, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

	linger.l_onoff = 1;
	linger.l_linger = 1;
	setsockopt(sockOut, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

	close(sockOut);
	close(sockIn);
#ifdef DEBUG
	{
		char buf[1024];
		char *p, *end;
		
		p = strstr(msg, "Reason:");
		end = strstr(p, "<P>");
		if (!p || !end)
			buf[0] = 0;
		else {
			strncpy(buf, p, end - p);
			buf[end - p] = 0;
		}
		printf("%d --- request error %s\n", sockIn, buf);
	}
#endif
}

void request_alarm(int i)
{
	sigjmp_buf *jmp;

	if ((jmp = pthread_getspecific(key_alarm))) {
		pthread_setspecific(key_alarm, NULL);
		siglongjmp(*jmp, 1);
	}
}

#define METHOD_GET		1
#define METHOD_POST		2
#define METHOD_HEAD		3

int process_request(int sockIn)
{
	char data[LDATA];
	char adr[LADR], *p;
	int ldata, lreq, port, req_len, req_method;
	FILE *fsin;
	sigjmp_buf timeout_jmp;
	int sockOut = -1;
	int val;
	
	/* let's reopen input socket as a file */
	if ((fsin = fdopen(sockIn, "rw")) == NULL)
		goto serverr;

	/* prepares for connection abort */
	/* avoid some sockets problems ... */
	val = 1;
	setsockopt(sockIn, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));
/* 
 * here, we'll analyze the request and get rid of "http://adr:port". 
 * The address and port willbe duplicated and used to open the connection.
 */
	if (sigsetjmp(timeout_jmp, 1) != 0)
		goto timeout;
	pthread_setspecific(key_alarm, &timeout_jmp);

#ifdef DEBUG
	printf("%d --- request begin\n", sockIn);
#endif
	if (fgets(data, LDATA, fsin) == NULL)
		goto badreq;
#ifdef DEBUG
	printf("%d %s", sockIn, data);
#endif
	/* it's easy to log all requests here */
	/* fprintf(stderr,"requete recue: %s",data);   */
	ldata = strlen(data);
	if (strncmp(data, "GET ", 4) == 0) {
		req_len = 4;
		req_method = METHOD_GET;
	} else if (strncmp(data, "POST ", 5) == 0) {
		req_len = 5;
		req_method = METHOD_POST;
	} else if (strncmp(data, "HEAD ", 5) == 0) {
		req_len = 5;
		req_method = METHOD_HEAD;
	} else
		goto badreq;

	if (!ConnectToProxy) {	/* if proxy-to-proxy connection, we don't modify the request */
		char *str;

		str = data + req_len;
		while (*str == ' ')
			str++;
		if (!strncmp(str, "http://", 7))
			str += 7;
		if ((p = strchr(str, '/')) != NULL) {
			strncpy(adr, str, (p - str));	/* copies addresse in adr */
			adr[p - str] = 0;
			str = p;	/* points to the rest of the request (without address) */
			lreq = ldata - (str - data);
		} else
			goto badreq;	/* if no /, error */
		/* at this stage, adr contains addr[:port], and str points to the local URL with the first  '/' */
		if (adr[0] == 0)
			goto badreq;
		p = strchr(adr, ':');
		if (p == NULL)	/* unspecified port. The default one will be used */
			port = DEFAULTPORT;
		else {		/* port is available. let's read it */
			*(p++) = 0;	/* ends hostname */
			port = atoi(p);
		}
		/* end of request analysis. The hostname is in "adr", and the port in "port" */
		if ((sockOut = connectto(adr, port, sockIn)) < 0) {
			switch (sockOut) {
				case ERR_GETHOSTBYNAME:
					goto servdnserr;
			}
			goto servcoerr;
		}
		/* As it becomes a local URL, we only say "GET" and the end of the request. */
		alarm(timeout_out);
		switch (req_method) {
		case METHOD_GET:
			write(sockOut, "GET ", 4);
			break;
		case METHOD_POST:
			write(sockOut, "POST ", 5);
			break;
		case METHOD_HEAD:
			write(sockOut, "HEAD ", 5);
			break;
		}
		write(sockOut, str, lreq);
		alarm(0);
	} else {		/* proxy-to-proxy connection ! */
		if ((sockOut = connectto(NextProxyAdr, NextProxyPort, sockIn)) < 0) {
			switch (sockOut) {
				case ERR_GETHOSTBYNAME:
					goto servdnserr;
			}
			goto servcoerr;
		}
		alarm(timeout_out);
		write(sockOut, data, ldata);
		alarm(0);
	}
	/* now, let's copy all what we don't have copied yet */
	if (req_method == METHOD_POST) {
		int c_len = 0;
		char *p;

		do {
			fgets(data, LDATA, fsin);
#ifdef DEBUG
			printf("%d %s", sockIn, data);
#endif
			ldata = strlen(data);
			if (strncasecmp(data, "Content-Length", 14) == 0) {
				p = data + 14;
				while (*p != ':')
					p++;
				c_len = atoi(++p);
			}
			write(sockOut, data, ldata);
		} while (ldata && data[0] != '\n' && data[0] != '\r');
		if (c_len == 0)
			goto posterr;
#ifdef DEBUG
		printf("%d ", sockIn);
#endif
		while (c_len) {
			ldata = fread(data, 1,
				  (LDATA > c_len ? c_len : LDATA), fsin);
#ifdef DEBUG
			fwrite(data, 1, ldata, stdout);
#endif
			write(sockOut, data, ldata);
			c_len -= ldata;
		}
#ifdef DEBUG
		printf("\n");
#endif
	} else { /*
		  * METHOD_GET, METHOD_HEAD
		  */
		do {
			fgets(data, LDATA, fsin);
#ifdef DEBUG
			printf("%d %s", sockIn, data);
#endif
			ldata = strlen(data);
			if (!NoCache || (strncmp(data, "If-Mod", 6)))
				write(sockOut, data, ldata);
		} while (ldata && data[0] != '\n' && data[0] != '\r');
	}
	/* retrieve data from server */
	do {
		int err;
		do {
			alarm(timeout_out);
			ldata = read(sockOut, data, LDATA);
			alarm(0);
		} while (ldata == -1 && errno == EINTR);	/* retry on interrupt */
		if (ldata < 0)
			goto serverr;
		if (ldata) {	/* if ldata > 0, it's not the end yet */
			do {
				err = write(sockIn, data, ldata);
			} while (err == -1 && errno == EINTR);
			if (errno == EPIPE) {	/* other end (client) closed the conection */
#ifdef DEBUG
				printf("%d   - client closed connection\n", sockIn);
#endif
				goto end;
			}
			if (err == -1)
				goto serverr;
		}
	} while (ldata > 0);	/* loops while more data available */

      end:
	close(sockIn);		/* close the sockets */
	close(sockOut);
	pthread_setspecific(key_alarm, NULL);
#ifdef DEBUG
	printf("%d --- request successful\n", sockIn);
#endif
	return 0;		/* no error */
      badreq:
	sayerror(BADREQ, sockIn, sockOut);
	return -1;
      serverr:
	sayerror(SERVERR, sockIn, sockOut);
	return -2;
      timeout:
	sayerror(SERVTIMEOUT, sockIn, sockOut);
	return -3;
      servcoerr:
	sayerror(SERVCOERR, sockIn, sockOut);
	return -4;
      servdnserr:
	sayerror(SERVDNSERR, sockIn, sockOut);
	return -5;
      posterr:
	sayerror(POSTERR, sockIn, sockOut);
	return -6;
}

void *client(struct th_proxy_struct *th_proxy)
{
	struct timespec ts;
	struct timeval tv;
	int retval;
	struct th_proxy_struct **th;

	signal(SIGPIPE, SIG_IGN);
	for (;;) {
		pthread_mutex_lock(&th_proxy->mu);
		while (th_proxy->sock_in < 0) {
#if 0
			pthread_cond_wait(&th_proxy->cond, &th_proxy->mu);
#else
			gettimeofday(&tv, NULL);
			ts.tv_sec = tv.tv_sec + TIMEOUT_THREAD_EXIT;
			ts.tv_nsec = 0;
			retval = pthread_cond_timedwait(
				    &th_proxy->cond, &th_proxy->mu, &ts);
			if (retval == ETIMEDOUT) {
				pthread_mutex_lock(&free_q_mu);
				th = &free_q;
				while (*th && *th != th_proxy)
					th = &((*th)->next_free);
				if (*th == th_proxy) {
					/*
					 * remove yourself from queue
					 * and exit
					 */
					*th = th_proxy->next_free;
					thread_count--;
					pthread_mutex_unlock(&free_q_mu);
					free(th_proxy);
					pthread_exit(0);
				}
				pthread_mutex_unlock(&free_q_mu);
			}
#endif
		}
		pthread_mutex_unlock(&th_proxy->mu);

		pthread_setspecific(key_alarm, (void *) 0);
		process_request(th_proxy->sock_in);

		pthread_mutex_lock(&th_proxy->mu);
		th_proxy->sock_in = -1;
		pthread_mutex_unlock(&th_proxy->mu);

		pthread_mutex_lock(&free_q_mu);
		th_proxy->next_free = free_q;
		free_q = th_proxy;
		pthread_mutex_unlock(&free_q_mu);
		pthread_cond_signal(&free_q_cond);
	}
}

/* displays the right syntax to call Webroute */
void displaysyntax(void)
{
	fprintf(stderr, "Syntax:\n");
	fprintf(stderr, "webroute [ -p port ] [ -x h:p ] [ -t timeout ] [ -m max_threads ] [ -n ]\n");
	fprintf(stderr, "Available options are:\n");
	fprintf(stderr, "  -p allows you to run webroute on the port <port>.\n");
	fprintf(stderr, "     If you don't have superuser privileges, you must use a port > 5000.\n");
	fprintf(stderr, "     The default port is %d.\n", LISTENPORT);
	fprintf(stderr, "  -x enables multi-proxy feature. This means that this instance of Webroute\n");
	fprintf(stderr, "     doesn't have itself access to the internet, but the one which is running\n");
	fprintf(stderr, "     on port <p> of host <h> can provide an access. It's possible to chain\n");
	fprintf(stderr, "     as many instances of Webroute as you want. That depends of your network\n");
	fprintf(stderr, "     topology\n");
	fprintf(stderr, "  -t <timeout> specifies how many seconds the connection will stay connected (or trying to connect) when\n");
	fprintf(stderr, "     no data arrives. After this time, the connection will be canceled.\n");
	fprintf(stderr, "  -n prevents browsers from retrieving web pages from their own cache when the\n");
	fprintf(stderr, "     user asks for a \"Reload\". The page will then always be reloaded.\n");
	fprintf(stderr, "  -m max count of proxy threads allocated to serve the requests\n");
}
