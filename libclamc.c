/******************************************************************************

	Clamav clamd client library

	Scanning file with tcp-based clamd servers. Library for general use.
	Thread-safe if compiled with _THREAD_SAFE defined.

	Warnings logged via syslog (must be opended).

	Random generator used (random() must be initialized before use, e.g.
	by srandomdev() call).

	Written by Maxim Dounin, mdounin@rambler-co.ru

	$Id$

******************************************************************************/

#ifdef _THREAD_SAFE
#include <pthread.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#include <syslog.h>

#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

#include "cfg_file.h"
#include "rmilter.h"
#include "libclamc.h"

/* Maximum time in seconds during which clamav server is marked inactive after scan error */
#define INACTIVE_INTERVAL 60.0
/* Maximum number of failed attempts before marking server as inactive */
#define MAX_FAILED 5
/* Maximum inactive timeout (20 min) */
#define MAX_TIMEOUT 1200.0


/* Global mutexes */

#ifdef _THREAD_SAFE
pthread_mutex_t mx_clamav_write = PTHREAD_MUTEX_INITIALIZER;
#endif

/*****************************************************************************/

/*
 * poll_fd() - wait for some POLLIN event on socket for timeout milliseconds.
 */

int poll_fd(int fd, int timeout, short events)
{
    int r, error;
    struct pollfd fds[1];
	socklen_t len;

	len = sizeof (error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len) == -1) {
		msg_warn("clamav: before poll(3) getsockopt, %d: %m", errno);
		return -1;
	}
	if (error != 0) {
		msg_warn("clamav: before poll(3) SO_ERROR on socket, %d: %s", error, strerror (error));
		return -1;
	}
	
    fds->fd = fd;
    fds->events = events;
    fds->revents = 0;
    while ((r = poll(fds, 1, timeout)) < 0) {
		if (errno != EINTR)
	    	break;
    }

	len = sizeof (error);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len) == -1) {
		msg_warn("clamav: after poll(3) getsockopt, %d: %m", errno);
		return -1;
	}
	if (error != 0) {
		msg_warn("clamav: after poll(3) SO_ERROR on socket, %d: %s", error, strerror (error));
		return -1;
	}

    return r;
}

/*
 * connect_t() - connect socket with timeout
 */

int connect_t(int s, const struct sockaddr *name, socklen_t namelen, int timeout)
{
    int r, ofl;
    int s_error = 0;
    socklen_t optlen;

    /* set nonblocking */
    ofl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, ofl | O_NONBLOCK);

    /* connect */
    r = connect(s, name, namelen);

    if (r < 0 && errno == EINPROGRESS) {
	/* wait for timeout */
		r = poll_fd(s, timeout, POLLOUT);
		if (r == 0) {
	    	r = -1;
	    	errno = ETIMEDOUT;
		} else if (r > 0) {
	    	/* check errors on socket, e. g. ECONNREFUSED */
	    	optlen = sizeof(s_error);
	    	/* XXX - errors in getsockopt are not checked, but it rarely fail */
	    	getsockopt(s, SOL_SOCKET, SO_ERROR, (void *)&s_error, &optlen);
	    	if (s_error) {
				r = -1;
				errno = s_error;
	    	}
		}
    }

    /* set blocking back */
    fcntl(s, F_SETFL, ofl);

    /* return */
    return r;
}


/*
 * clamscan_socket() - send file to specified host. See clamscan() for
 * load-balanced wrapper.
 * 
 * returns 0 when checked, -1 on some error during scan (try another server), -2
 * on unexpected error (probably clamd died on our file, fallback to another
 * host not recommended)
 */

int 
clamscan_socket(const char *file, const struct clamav_server *srv, char *strres, size_t strres_len)
{
    char path[MAXPATHLEN], buf[MAXPATHLEN + 10], *c;
    struct sockaddr_un server_un;
    struct sockaddr_in server_in, server_w;
    int s, sw, r, fd, port = 0, path_len;

    *strres = '\0';

    /* somebody doesn't need reply... */
    if (!srv)
		return 0;

    if (srv->sock_type == AF_LOCAL) {
		/* unix socket, use 'SCAN <filename>' command on clamd */
		snprintf(buf, sizeof(buf), "SCAN %s\n", file);

		if (!realpath(file, path)) {
	    	msg_warn("clamav: realpath, %d: %m", errno);
	    	return -1;
		}
		memset(&server_un, 0, sizeof(server_un));
		server_un.sun_family = AF_UNIX;
		strncpy(server_un.sun_path, srv->sock.unix_path, sizeof(server_un.sun_path));

		if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	    	msg_warn("clamav: socket %s, %d: %m", srv->sock.unix_path, errno);
	    	return -1;
		}
		if (connect_t(s, (struct sockaddr *) & server_un, sizeof(server_un), 1000) < 0) {
	    	msg_warn("clamav: connect %s, %d: %m", srv->sock.unix_path, errno);
	    	close(s);
	    	return -1;
		}
		if (write(s, buf, strlen(buf)) != strlen(buf)) {
	    	msg_warn("clamav: write %s, %d: %m", srv->sock.unix_path, errno);
	    	close(s);
	    	return -1;
		}

    } else {
		/* inet hostname, send stream over tcp/ip */

		memset(&server_in, 0, sizeof(server_in));
		server_in.sin_family = AF_INET;
		server_in.sin_port = srv->sock.inet.port;
		memcpy((char *)&server_in.sin_addr, &srv->sock.inet.addr, sizeof(struct in_addr));

		if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
	    	msg_warn("clamav: socket %d: %m",  errno);
	    	return -1;
		}
		if (connect_t(s, (struct sockaddr *) & server_in, sizeof(server_in), 1000) < 0) {
	    	msg_warn("clamav: connect %d: %m", errno);
	    	close(s);
	    	return -1;
		}
		snprintf(path, MAXPATHLEN, "stream");
		snprintf(buf, MAXPATHLEN, "STREAM");

		if (write(s, buf, strlen(buf)) != strlen(buf)) {
	    	msg_warn("clamav: write %d: %m", errno);
	    	close(s);
	    	return -1;
		}
		if (poll_fd(s, 5000, POLLIN) < 1) {
	    	msg_warn("clamav: timeout waiting port");
	    	close(s);
	    	return -1;
		}

		/* clamav must reply with PORT to connect */

		buf[0] = 0;
		if ((r = read(s, buf, MAXPATHLEN)) > 0)
	    	buf[r] = 0;

		if (strncmp(buf, "PORT ", 5) == 0)
	    	port = atoi(buf + 5);

		if (port < 1024) {
	    	msg_warn("clamav: can't get port number for data stream, got: %s", buf);
	    	close(s);
	    	return -1;
		}

		/*
		 * connect to clamd data socket
	 	 */
		if ((sw = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	    	msg_warn("clamav: socket, %d: %m", errno);
	    	close(s);
	    	return -1;
		}
		memset(&server_w, 0, sizeof(server_w));
		server_w.sin_family = AF_INET;
		server_w.sin_port = htons(port);
		memcpy((char *)&server_w.sin_addr, (char *)&server_in.sin_addr, sizeof(struct in_addr));

		if (connect_t(sw, (struct sockaddr *) & server_w, sizeof(server_w), 15000) < 0) {
	    	msg_warn("clamav: connect data, %d: %m", errno);
	    	close(sw);
	    	close(s);
	    	return -1;
		}

		/*
	 	 * send data stream
	 	 */

		fd = open(file, O_RDONLY);
		if (sendfile(fd, sw, 0, 0, 0, 0, 0) != 0) {
	    	msg_warn("clamav: sendfile, %d: %m", errno);
	    	close(fd);
	    	close(sw);
	    	close(s);
	    	return -1;
		}
		close(fd);
		shutdown(sw, SHUT_RDWR);
		close(sw);

    }

    /* wait for reply, timeout in 15 seconds */

    if (poll_fd(s, 15000, POLLIN) < 1) {
		msg_warn("clamav: timeout waiting results");
		close(s);
		return -1;
    }
	
    /*
     * read results
     */

    buf[0] = 0;

    while ((r = read(s, buf, MAXPATHLEN)) > 0) {
		buf[r] = 0;
    }

    if (r < 0) {
		msg_warn("clamav: read, %s, %d: %m", (srv->sock_type == AF_UNIX) ? srv->sock.unix_path : srv->sock.inet.addr_str, errno);
		close(s);
		return -1;
    }

    close(s);

    /*
     * ok, we got result; test what we got
     */

    /* msg_warn("clamav: %s", buf); */
    if ((c = strstr(buf, "OK\n")) != NULL) {
	/* <file> ": OK\n" */
		return 0;

    } else if ((c = strstr(buf, "FOUND\n")) != NULL) {
	/* <file> ": " <virusname> " FOUND\n" */

		path_len = strlen(path);
		if (strncmp(buf, path, path_len) != 0) {
	    	msg_warn("clamav: paths differ: '%s' instead of '%s'", buf, path);
	    	return -1;
		}
		*(--c) = 0;
		c = buf + path_len + 2;

	   /*
	 	* Virus found, store in state to further checks with
	 	* smtpd_dot_restrictions = check_clamd_access pcre:/db/maps/clamd
	 	* (in postfix).
	 	*/

		/* msg_warn("clamav: found %s", c); */
		snprintf(strres, strres_len, "%s", c);
		return 0;

    } else if ((c = strstr(buf, "ERROR\n")) != NULL) {
		*(--c) = 0;
		msg_warn("clamav: error %s", buf);
		return -1;
    }

    /*
     * Most common reason is clamd died while processing our request. Try to
     * save file for further investigation and fail.
     */

    msg_warn("clamav: unexpected result on file %s, %s", file, buf);
    return -2;
}

/*
 * clamscan() - send file to one of remote clamd, with pseudo load-balancing
 * (select one random server, fallback to others in case of errors).
 * 
 * returns 0 if file scanned (or not scanned due to filesize limit), -1 when
 * retry limit exceeded, -2 on unexpected error, e.g. unexpected reply from
 * server (suppose scanned message killed clamd...)
 */

int 
clamscan(const char *file, struct config_file *cfg, char *strres, size_t strres_len)
{
    int retry = 5, r = -2;
    /* struct stat sb; */
    struct timeval t;
    double ts, tf;
    struct clamav_server *selected = NULL;
	int k;

    *strres = '\0';
    /*
     * Parse sockets to use in balancing.
     */
    /* msg_warn("(clamscan) defined %d server sockets...", sockets_n); */

    /*
     * save scanning start time
     */
    gettimeofday(&t, NULL);
    ts = t.tv_sec + t.tv_usec / 1000000.0;

    /* try to scan with available servers */
    while (1) {
		k = random() % cfg->clamav_servers_num;
		/* Minimum number of requests - set to maximum available value */
		selected = &cfg->clamav_servers[k];
#if 0
		/* Check inactive timeout */
		if (selected->active == 0 && ts >= selected->next_check) {
			msg_info ("(clamscan) remarking server as active due to timeout");
			pthread_mutex_lock (&mx_clamav_write);
			selected->active = 1;
			cfg->clamav_servers_alive++;
			pthread_mutex_unlock (&mx_clamav_write);
		}
		/* No active servers were found */
		if (cfg->clamav_servers_alive == 0) {
			msg_warn ("(clamscan) all servers are marked as inactive, trying to restore all");
			pthread_mutex_lock (&mx_clamav_write);
			for(k = 0; k < cfg->clamav_servers_num; k++) {
				cfg->clamav_servers[k].active = 1;
			}
			cfg->clamav_servers_alive = cfg->clamav_servers_num;
			pthread_mutex_unlock (&mx_clamav_write);
			continue;
		}
		if (selected->active == 0) {
			continue;
		}
#endif

		r = clamscan_socket (file, selected, strres, strres_len);
		if (r == 0) {
/*
			pthread_mutex_lock (&mx_clamav_write);
			selected->failed_attempts = 0;
			pthread_mutex_unlock (&mx_clamav_write);
*/
	    	break;
		}
		if (r == -2) {
	    	msg_warn("(clamscan) unexpected problem, %s", file);
	    	break;
		}
		if (--retry < 1) {
	    	msg_warn("(clamscan) retry limit exceeded, %s", file);
	    	break;
		}
		msg_warn("(clamscan) failed to scan, retry, %s", file);
#if 0
		/* Increment failed attempts counter */
		pthread_mutex_lock (&mx_clamav_write);
		selected->failed_attempts++;
		/* Mark server inactive */
		if (selected->failed_attempts > MAX_FAILED) {
			msg_warn ("(clamscan) marking clamav server as inactive after %d failed scan attempts", selected->failed_attempts);
			selected->active = 0;
			tf = ts + INACTIVE_INTERVAL * (selected->failed_attempts - MAX_FAILED);
			selected->next_check = (tf - ts) > MAX_TIMEOUT ? (ts + MAX_TIMEOUT) : tf;
			cfg->clamav_servers_alive--;
		}
		pthread_mutex_unlock (&mx_clamav_write);
#endif
		sleep(1);
    }

    /*
     * print scanning time, server and result
     */
    gettimeofday(&t, NULL);
    tf = t.tv_sec + t.tv_usec / 1000000.0;

    if (*strres) {
		msg_info("(clamscan) scan %f, %s, found %s, %s", tf - ts,
					(selected->sock_type == AF_UNIX) ? selected->sock.unix_path : selected->sock.inet.addr_str, 
					strres, file);
	}
    else {
		msg_info("(clamscan) scan %f, %s, %s", tf -ts, 
					(selected->sock_type == AF_UNIX) ? selected->sock.unix_path : selected->sock.inet.addr_str,
					file);
	}

    return r;
}
