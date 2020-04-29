#include "device.h"
#include "track.h"
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

#if defined(WIN32) || defined(WIN64) || defined(_XBOX)
 #include <direct.h>
 #define S_ISDIR(m) (((m)& S_IFMT) == S_IFDIR)
 #define mkdir(pathname, mode) _mkdir(pathname)
#elif defined(GEKKO)
 #include <network.h>
#endif

#ifdef _XBOX
#define strdup _strdup
#define stat _stat
#endif
static int find_track(struct sync_device *d, const char *name)
{
	int i;
	for (i = 0; i < (int)d->num_tracks; ++i)
		if (!strcmp(name, d->tracks[i]->name))
			return i;
	return -1; /* not found */
}

static int valid_path_char(char ch)
{
	switch (ch) {
	case '.':
	case '_':
	case '/':
#if defined(WIN32) || defined(_XBOX)
	case '\\':
#endif
		return 1;

	default:
#ifdef _XBOX
		return isalnum(((int)ch));
#else
		return isalnum(ch);
#endif
	}
}

static char index_for_ch(char ch)
{
	int i;
	for (i = 0; i < (int)((sizeof("0123456789ABCDEF") - 1u)); ++i) {
		if (ch == "0123456789ABCDEF"[i])
			return (char)i;
	}
	return (char)i;
}

const char *sync_path_encode(const char *path, char *tgt, size_t tgtSizeInChars)
{
	int i;
	size_t pos = 0u;
	int path_len = (int)strlen(path);
	for (i = 0; i < path_len; ++i) {
		int ch = path[i];
		if (valid_path_char(ch)) {
			if (pos >= (tgtSizeInChars - 1u))
				break;

			tgt[pos++] = (char)ch;
		}
		else {
			if (pos >= (tgtSizeInChars - 3u))
				break;

			tgt[pos++] = '-';
			tgt[pos++] = "0123456789ABCDEF"[(ch >> 4) & 0xF];
			tgt[pos++] = "0123456789ABCDEF"[ch & 0xF];
		}
	}

	tgt[pos] = '\0';
	return tgt;
}

const char *sync_path_decode(const char *path, char *tgt, size_t tgtSizeInChars)
{
	size_t len = strlen(path);
	size_t i = 0u;
	size_t pos = 0u;
	if (len >= tgtSizeInChars)
		len = tgtSizeInChars - 1u;
	while (i < len) {
		char ch = path[i];
		if (('-' == ch) && (i < (len - 2u))) {
			char upper = index_for_ch(path[i + 1u]);
			char lower = index_for_ch(path[i + 2u]);
			char finalCh = (upper << 4) | lower;
			if (0 == valid_path_char(finalCh)) {
				tgt[pos++] = finalCh;
				i += 3u;
				continue;
			}
		}
		tgt[pos++] = ch;
		++i;
	}
	tgt[pos] = '\0';
	return tgt;
}

static const char *path_encode(const char *path)
{
	static char temp[FILENAME_MAX];
	return sync_path_encode(path, temp, sizeof(temp));
}

static const char *sync_track_path(const char *base, const char *name)
{
	static char temp[FILENAME_MAX];
	strncpy(temp, base, sizeof(temp) - 1);
	temp[sizeof(temp) - 1] = '\0';
	strncat(temp, "_", sizeof(temp) - strlen(temp) - 1);
	strncat(temp, path_encode(name), sizeof(temp) - strlen(temp) - 1);
	strncat(temp, ".track", sizeof(temp) - strlen(temp) - 1);
	return temp;
}

#ifndef SYNC_PLAYER

#define CLIENT_GREET "hello, synctracker!"
#define SERVER_GREET "hello, demo!"

enum {
	SET_KEY = 0,
	DELETE_KEY = 1,
	GET_TRACK = 2,
	SET_ROW = 3,
	PAUSE = 4,
	SAVE_TRACKS = 5
};

static inline int socket_poll(SOCKET socket)
{
#ifdef GEKKO
	// libogc doesn't impmelent select()...
	struct pollsd sds[1];
	sds[0].socket  = socket;
	sds[0].events  = POLLIN;
	sds[0].revents = 0;
	if (net_poll(sds, 1, 0) < 0) return 0;
	return (sds[0].revents & POLLIN) && !(sds[0].revents & (POLLERR|POLLHUP|POLLNVAL));
#else
	struct timeval to = { 0, 0 };
	fd_set fds;

	FD_ZERO(&fds);

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4127)
#endif
	FD_SET(socket, &fds);
#ifdef _MSC_VER
#pragma warning(pop)
#endif

	return select((int)socket + 1, &fds, NULL, NULL, &to) > 0;
#endif
}

static inline int xsend(SOCKET s, const void *buf, size_t len, int flags)
{
#if defined(WIN32) || defined(_XBOX)
	assert(len <= INT_MAX);
	return send(s, (const char *)buf, (int)len, flags) != (int)len;
#else
	return send(s, (const char *)buf, len, flags) != (int)len;
#endif
}

static inline int xrecv(SOCKET s, void *buf, size_t len, int flags)
{
#if defined(WIN32) || defined(_XBOX)
	assert(len <= INT_MAX);
	return recv(s, (char *)buf, (int)len, flags) != (int)len;
#else
	return recv(s, (char *)buf, len, flags) != (int)len;
#endif
}

#ifdef USE_AMITCP
static struct Library *socket_base = NULL;
#endif

static SOCKET server_connect(const char *host, unsigned short nport)
{
	int res;
	int protocol = 0;
	SOCKET sock = INVALID_SOCKET;
#ifdef USE_GETADDRINFO
	struct addrinfo *addr, *curr;
	char port[6];
#elif defined(_XBOX)
	int i;
	const int family = AF_INET;
	const int sa_len = (int)sizeof(struct sockaddr);
	struct sockaddr_in addr;
	struct sockaddr *sa = (struct sockaddr*)&addr;
	protocol = IPPROTO_TCP;
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(host);
	addr.sin_port = htons(nport);
#else
	struct hostent *he;
	char **ap;
#endif

#if defined(WIN32) || defined(_XBOX)
	static int need_init = 1;
	if (need_init) {
#ifdef _XBOX
		XNetStartupParams params;
		// start xbox net
		// move this to the engine side if net is needed for other things
		memset(&params, 0, sizeof(XNetStartupParams));
		params.cfgSizeOfStruct = sizeof(XNetStartupParams);
		params.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
		if (XNetStartup(&params) != NO_ERROR) {
			fprintf(stderr, "rocket: XNetStartup() failed\n");
			return INVALID_SOCKET;
		}
#endif
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 0), &wsa)) {
			fprintf(stderr, "rocket: WSAStartup() failed\n");
			return INVALID_SOCKET;
		}
		need_init = 0;
	}
#elif defined(USE_AMITCP)
	if (!socket_base) {
		socket_base = OpenLibrary("bsdsocket.library", 4);
		if (!socket_base)
			return INVALID_SOCKET;
	}
#endif

#ifdef USE_GETADDRINFO

	snprintf(port, sizeof(port), "%u", nport);
	if (getaddrinfo(host, port, 0, &addr) != 0)
		return INVALID_SOCKET;

	for (curr = addr; curr; curr = curr->ai_next) {
		int family = curr->ai_family;
		struct sockaddr *sa = curr->ai_addr;
		int sa_len = (int)curr->ai_addrlen;
#elif defined(_XBOX)
	for (i = 0; i < 1; ++i) {
#else

	he = gethostbyname(host);
	if (!he)
		return INVALID_SOCKET;

	for (ap = he->h_addr_list; *ap; ++ap) {
		int family = he->h_addrtype;
		struct sockaddr_in sin;
		struct sockaddr *sa = (struct sockaddr *)&sin;
		int sa_len = sizeof(*sa);

		sin.sin_family = he->h_addrtype;
		sin.sin_port = htons(nport);
		memcpy(&sin.sin_addr, *ap, he->h_length);
		memset(&sin.sin_zero, 0, sizeof(sin.sin_zero));

#endif
		sock = socket(family, SOCK_STREAM, protocol);
		if (sock == INVALID_SOCKET) {
			fprintf(stderr, "rocket: socket() failed\n");
			continue;
		}
		res = connect(sock, sa, sa_len);
		if (res >= 0) {
			char greet[128];

			if (xsend(sock, CLIENT_GREET, strlen(CLIENT_GREET), 0) ||
				xrecv(sock, greet, strlen(SERVER_GREET), 0)) {
				closesocket(sock);
				sock = INVALID_SOCKET;
				continue;
			}

			if (!strncmp(SERVER_GREET, greet, strlen(SERVER_GREET)))
				break;
		} else {
#if defined(WIN32) || defined(_XBOX)
			fprintf(stderr, "rocket: connect() failed: %d. last error: %d\n",
					res, (int)WSAGetLastError());
#else
			fprintf(stderr, "rocket: connect() failed: %d\n", res);
#endif
		}

		closesocket(sock);
		sock = INVALID_SOCKET;
	}

#ifdef USE_GETADDRINFO
	freeaddrinfo(addr);
#endif

	return sock;
}

#else

void sync_set_io_cb(struct sync_device *d, struct sync_io_cb *cb)
{
	d->io_cb.open = cb->open;
	d->io_cb.read = cb->read;
	d->io_cb.close = cb->close;
}

#endif

#ifdef NEED_STRDUP
static inline char *rocket_strdup(const char *str)
{
	char *ret = malloc(strlen(str) + 1);
	if (ret)
		strcpy(ret, str);
	return ret;
}
#define strdup rocket_strdup
#endif

struct sync_device *sync_create_device(const char *base)
{
	struct sync_device *d = malloc(sizeof(*d));
	if (!d)
		return NULL;

	if (!base || base[0] == '/')
		return NULL;

	d->base = strdup(path_encode(base));
	if (!d->base) {
		free(d);
		return NULL;
	}

	d->tracks = NULL;
	d->num_tracks = 0;

#ifndef SYNC_PLAYER
	d->row = -1;
	d->sock = INVALID_SOCKET;
#endif

	d->io_cb.open = (void *(*)(const char *, const char *))fopen;
	d->io_cb.read = (size_t (*)(void *, size_t, size_t, void *))fread;
	d->io_cb.close = (int (*)(void *))fclose;

	return d;
}

void sync_destroy_device(struct sync_device *d)
{
	int i;

#ifndef SYNC_PLAYER
	if (d->sock != INVALID_SOCKET)
		closesocket(d->sock);
#ifdef _XBOX
	WSACleanup();
	XNetCleanup();
#endif
#endif

	for (i = 0; i < (int)d->num_tracks; ++i) {
		free(d->tracks[i]->name);
		free(d->tracks[i]->keys);
		free(d->tracks[i]);
	}
	free(d->tracks);
	free(d->base);
	free(d);

#if defined(USE_AMITCP) && !defined(SYNC_PLAYER)
	if (socket_base) {
		CloseLibrary(socket_base);
		socket_base = NULL;
	}
#endif
}

static int read_track_data(struct sync_device *d, struct sync_track *t)
{
	int i;
	void *fp = d->io_cb.open(sync_track_path(d->base, t->name), "rb");
	if (!fp)
		return -1;

	d->io_cb.read(&t->num_keys, sizeof(int), 1, fp);
	t->keys = malloc(sizeof(struct track_key) * t->num_keys);
	if (!t->keys)
		return -1;

	for (i = 0; i < (int)t->num_keys; ++i) {
		struct track_key *key = t->keys + i;
		char type;
		d->io_cb.read(&key->row, sizeof(int), 1, fp);
		d->io_cb.read(&key->value, sizeof(float), 1, fp);
		d->io_cb.read(&type, sizeof(char), 1, fp);
		key->type = (enum key_type)type;
	}

	d->io_cb.close(fp);
	return 0;
}

static int create_leading_dirs(const char *path)
{
	char *pos, buf[FILENAME_MAX];

	strncpy(buf, path, sizeof(buf));
	buf[sizeof(buf) - 1] = '\0';
	pos = buf;

	while (1) {
		struct stat st;

		pos = strchr(pos, '/');
		if (!pos)
			break;
#if defined(WIN32) || defined(_XBOX)
		pos = strchr(pos, '\\');
		if (!pos)
			break;
#endif
		*pos = '\0';

		/* does path exist, but isn't a dir? */
		if (!stat(buf, &st)) {
			if (!S_ISDIR(st.st_mode))
				return -1;
		} else {
			if (mkdir(buf, 0777))
				return -1;
		}
#if defined(WIN32) || defined(_XBOX)
		*pos++ = '\\';
#else
		*pos++ = '/';
#endif
	}

	return 0;
}

static int save_track(const struct sync_track *t, const char *path)
{
	int i;
	FILE *fp;

	if (create_leading_dirs(path))
		return -1;

	fp = fopen(path, "wb");
	if (!fp)
		return -1;

	fwrite(&t->num_keys, sizeof(int), 1, fp);
	for (i = 0; i < (int)t->num_keys; ++i) {
		char type = (char)t->keys[i].type;
		fwrite(&t->keys[i].row, sizeof(int), 1, fp);
		fwrite(&t->keys[i].value, sizeof(float), 1, fp);
		fwrite(&type, sizeof(char), 1, fp);
	}

	fclose(fp);
	return 0;
}

int sync_save_tracks(const struct sync_device *d)
{
	int i;
	for (i = 0; i < (int)d->num_tracks; ++i) {
		const struct sync_track *t = d->tracks[i];
		if (save_track(t, sync_track_path(d->base, t->name)))
			return -1;
	}
	return 0;
}

#ifndef SYNC_PLAYER

static int fetch_track_data(struct sync_device *d, struct sync_track *t)
{
	unsigned char cmd = GET_TRACK;
	uint32_t name_len;

	assert(strlen(t->name) <= UINT32_MAX);
	name_len = htonl((uint32_t)strlen(t->name));

	/* send request data */
	if (xsend(d->sock, (char *)&cmd, 1, 0) ||
	    xsend(d->sock, (char *)&name_len, sizeof(name_len), 0) ||
	    xsend(d->sock, t->name, (int)strlen(t->name), 0))
	{
		closesocket(d->sock);
		d->sock = INVALID_SOCKET;
		return -1;
	}

	return 0;
}

static int handle_set_key_cmd(SOCKET sock, struct sync_device *data)
{
	uint32_t track, row;
	union {
		float f;
		uint32_t i;
	} v;
	struct track_key key;
	unsigned char type;

	if (xrecv(sock, (char *)&track, sizeof(track), 0) ||
	    xrecv(sock, (char *)&row, sizeof(row), 0) ||
	    xrecv(sock, (char *)&v.i, sizeof(v.i), 0) ||
	    xrecv(sock, (char *)&type, 1, 0))
		return -1;

	track = ntohl(track);
	v.i = ntohl(v.i);

	key.row = ntohl(row);
	key.value = v.f;

	if (type >= KEY_TYPE_COUNT || track >= data->num_tracks)
		return -1;

	key.type = (enum key_type)type;
	return sync_set_key(data->tracks[track], &key);
}

static int handle_del_key_cmd(SOCKET sock, struct sync_device *data)
{
	uint32_t track, row;

	if (xrecv(sock, (char *)&track, sizeof(track), 0) ||
	    xrecv(sock, (char *)&row, sizeof(row), 0))
		return -1;

	track = ntohl(track);
	row = ntohl(row);

	if (track >= data->num_tracks)
		return -1;

	return sync_del_key(data->tracks[track], row);
}

int sync_tcp_connect(struct sync_device *d, const char *host, unsigned short port)
{
	int i;
	if (d->sock != INVALID_SOCKET)
		closesocket(d->sock);

	d->sock = server_connect(host, port);
	if (d->sock == INVALID_SOCKET)
		return -1;

	for (i = 0; i < (int)d->num_tracks; ++i) {
		free(d->tracks[i]->keys);
		d->tracks[i]->keys = NULL;
		d->tracks[i]->num_keys = 0;
	}

	for (i = 0; i < (int)d->num_tracks; ++i) {
		if (fetch_track_data(d, d->tracks[i])) {
			closesocket(d->sock);
			d->sock = INVALID_SOCKET;
			return -1;
		}
	}
	return 0;
}

int sync_connect(struct sync_device *d, const char *host, unsigned short port)
{
	return sync_tcp_connect(d, host, port);
}

int sync_update(struct sync_device *d, int row, struct sync_cb *cb,
    void *cb_param)
{
	if (d->sock == INVALID_SOCKET)
		return -1;

	/* look for new commands */
	while (socket_poll(d->sock)) {
		unsigned char cmd = 0, flag;
		uint32_t new_row;
		if (xrecv(d->sock, (char *)&cmd, 1, 0))
			goto sockerr;

		switch (cmd) {
		case SET_KEY:
			if (handle_set_key_cmd(d->sock, d))
				goto sockerr;
			break;
		case DELETE_KEY:
			if (handle_del_key_cmd(d->sock, d))
				goto sockerr;
			break;
		case SET_ROW:
			if (xrecv(d->sock, (char *)&new_row, sizeof(new_row), 0))
				goto sockerr;
			if (cb && cb->set_row)
				cb->set_row(cb_param, ntohl(new_row));
			break;
		case PAUSE:
			if (xrecv(d->sock, (char *)&flag, 1, 0))
				goto sockerr;
			if (cb && cb->pause)
				cb->pause(cb_param, flag);
			break;
		case SAVE_TRACKS:
			sync_save_tracks(d);
			break;
		default:
			fprintf(stderr, "unknown cmd: %02x\n", cmd);
			goto sockerr;
		}
	}

	if (cb && cb->is_playing && cb->is_playing(cb_param)) {
		if (d->row != row && d->sock != INVALID_SOCKET) {
			unsigned char cmd = SET_ROW;
			uint32_t nrow = htonl(row);
			if (xsend(d->sock, (char*)&cmd, 1, 0) ||
			    xsend(d->sock, (char*)&nrow, sizeof(nrow), 0))
				goto sockerr;
			d->row = row;
		}
	}
	return 0;

sockerr:
	closesocket(d->sock);
	d->sock = INVALID_SOCKET;
	return -1;
}

#endif /* !defined(SYNC_PLAYER) */

static int create_track(struct sync_device *d, const char *name)
{
	void *tmp;
	struct sync_track *t;
	assert(find_track(d, name) < 0);

	t = malloc(sizeof(*t));
	if (!t)
		return -1;

	t->name = strdup(name);
	t->keys = NULL;
	t->num_keys = 0;

	tmp = realloc(d->tracks, sizeof(d->tracks[0]) * (d->num_tracks + 1));
	if (!tmp) {
		free(t);
		return -1;
	}

	d->tracks = tmp;
	d->tracks[d->num_tracks++] = t;

	return (int)d->num_tracks - 1;
}

const struct sync_track *sync_get_track(struct sync_device *d,
    const char *name)
{
	struct sync_track *t;
	int idx = find_track(d, name);
	if (idx >= 0)
		return d->tracks[idx];

	idx = create_track(d, name);
	if (idx < 0)
		return NULL;

	t = d->tracks[idx];

#ifndef SYNC_PLAYER
	if (d->sock != INVALID_SOCKET)
		fetch_track_data(d, t);
	else
#endif
		read_track_data(d, t);

	return t;
}
