 /* See LICENSE file for license details. */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gotr.h"

struct link {
	char *name;
	void *cls;
	struct link* users;
	struct link* next;
};

static struct link* rooms;
static char *host = "irc.oftc.net";
static char *port = "6667";
static char *password;
static char nick[32];
static char bufin[4096];
static char bufout[4096];
static char channel[256];
static time_t trespond;
static FILE *srv = NULL;

#include "util.c"

static void
pout(char *channel, char *fmt, ...) {
	static char timestr[18];
	time_t t;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(bufout, sizeof bufout, fmt, ap);
	va_end(ap);
	t = time(NULL);
	strftime(timestr, sizeof timestr, "%D %R", localtime(&t));
	fprintf(stdout, "%-12s: %s %s\n", channel, timestr, bufout);
}

static void
sout(char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(bufout, sizeof bufout, fmt, ap);
	va_end(ap);
	fprintf(srv, "%s\r\n", bufout);
}

static struct link*
lget(struct link* first, const char* name)
{
	struct link* cur;
	for (cur = first; cur; cur = cur->next)
		if (!strcmp(cur->name, name))
			return cur;
	return NULL;
}

static void*
derive(const struct link* first, const char* name)
{
	const struct link *cur;
	for (cur = first; cur; cur = cur->next)
		if (!strcmp(cur->name, name))
			return cur->cls;
	return NULL;
}

static struct link*
lrem(struct link** first, const char* name)
{
	struct link* cur;
	struct link** prev = first;
	for (cur = *first; cur; prev = &(cur->next), cur = cur->next)
		if (!strcmp(name, cur->name)) {
			*prev = cur->next;
			return cur;
		}
	return NULL;
}

static int
send_user(void* room, void* user, const char* message)
{
	const size_t nl = strlen(message) + strlen(room) + 2;
	char *nm = malloc(nl);
	snprintf(nm, nl, "%s %s", (char*)room, message);
	sout("PRIVMSG %s :%s", (char*)user, nm);
	free(nm);
	return 1;
}

static int
send_room(void* room, const char* message)
{
	sout("PRIVMSG %s :%s", (char*)room, message);
	return 1;
}

static void
receive_user(void *room, void *user, const char *message)
{
	pout((char*)room, "<%s> \033[32m%s\033[0m", (char*)user, message);
}

static void
privmsg(char *channel, char *msg) {
	if(channel[0] == '\0') {
		pout("", "No channel to send to");
		return;
	} else if(channel[0] == '#') {
		gotr_send(derive(rooms, channel), msg);
	} else {
		sout("PRIVMSG %s :%s", channel, msg);
	}
	pout(channel, "<%s> %s", nick, msg);
}

static void
parsein(char *s) {
	char c, *p;

	if(s[0] == '\0')
		return;
	skip(s, '\n');
	if(s[0] != ':') {
		privmsg(channel, s);
		return;
	}
	c = *++s;
	if(c != '\0' && isspace(s[1])) {
		p = s + 2;
		switch(c) {
		case 'j':
			sout("JOIN %s", p);
			if(channel[0] == '\0')
				strlcpy(channel, p, sizeof channel);
			return;
		case 'l':
			s = eat(p, isspace, 1);
			p = eat(s, isspace, 0);
			if(!*s)
				s = channel;
			if(*p)
				*p++ = '\0';
			if(!*p)
				p = "sic - 250 LOC are too much!";
			sout("PART %s :%s", s, p);
			return;
		case 'm':
			s = eat(p, isspace, 1);
			p = eat(s, isspace, 0);
			if(*p)
				*p++ = '\0';
			privmsg(s, p);
			return;
		case 'r':
			s = eat(p, isspace, 1);
			p = eat(s, isspace, 0);
			if(!*s)
				s = channel;
			if(*p)
				*p++ = '\0';
			gotr_rekey(derive(rooms, s), NULL);
			return;
		case 's':
			strlcpy(channel, p, sizeof channel);
			return;
		}
	}
	sout("%s", s);
}

static void
parsesrv(char *cmd) {
	char *usr, *par, *txt;
	struct link* lnk;
	struct link* room;
	struct gotr_user* user;

	usr = host;
	if(!cmd || !*cmd)
		return;
	if(cmd[0] == ':') {
		usr = cmd + 1;
		cmd = skip(usr, ' ');
		if(cmd[0] == '\0')
			return;
		skip(usr, '!');
	}
	skip(cmd, '\r');
	par = skip(cmd, ' ');
	txt = skip(par, ':');
	trim(par);
	if(!strcmp("PONG", cmd))
		return;
	if(!strcmp("PRIVMSG", cmd)) {
//		pout(par, "<%s> %s", usr, txt);
		if(txt[0] == '#') {
			par = txt;
			txt = skip(eat(par, isspace, 0), ' ');
			if (!(room = lget(rooms, par)))
				return;
			if (!(user = derive(room->users, usr))) {
				lnk = malloc(sizeof(struct link));
				lnk->name = malloc(strlen(usr) + 1);
				strncpy(lnk->name, usr, strlen(usr) + 1);
				lnk->next = room->users;
				if ((lnk->cls = gotr_receive_user(room->cls, NULL, lnk->name, txt))) {
					room->users = lnk;
				} else {
					free(lnk->name);
					free(lnk);
				}
			} else {
				gotr_receive_user(room->cls, user, usr, txt);
			}
		} else {
			gotr_receive(derive(rooms, par), txt);
		}
	} else if(!strcmp("PING", cmd)) {
		sout("PONG %s", txt);
	} else {
		pout(usr, ">< %s (%s): %s", cmd, par, txt);
		if (!strcmp(usr, nick) && !strcmp("JOIN", cmd)) {
			pout(txt, "joining");
			lnk = malloc(sizeof(struct link));
			lnk->users = NULL;
			lnk->name = malloc(strlen(txt) + 1);
			strncpy(lnk->name, txt, strlen(txt) + 1);
			lnk->next = rooms;
			if ((lnk->cls = gotr_join(&send_room, &send_user, &receive_user, lnk->name, NULL))) {
				rooms = lnk;
			} else {
				free(lnk->name);
				free(lnk);
			}
		} else if (!strcmp("JOIN", cmd)) {
			pout(txt, "%s joined", usr);
			if (!(room = lget(rooms, txt)))
				return;
			lnk = malloc(sizeof(struct link));
			lnk->users = NULL;
			lnk->name = malloc(strlen(usr) + 1);
			strncpy(lnk->name, usr, strlen(usr) + 1);
			lnk->next = room->users;
			if ((lnk->cls = gotr_user_joined(derive(rooms, txt), lnk->name))) {
				room->users = lnk;
			} else {
				free(lnk->name);
				free(lnk);
			}
		} else if (!strcmp(usr, nick) && !strcmp("PART", cmd)) {
			pout(par, "leaving");
			lnk = lrem(&rooms, par);
			gotr_leave(lnk->cls);
			free(lnk->name);
			free(lnk);
		} else if (!strcmp("PART", cmd)) {
			pout(par, "%s left", usr);
			if (!(room = lget(rooms, par)))
				return;
			lnk = lrem(&room->users, usr);
			gotr_user_left(room->cls, lnk->cls);
			free(lnk->name);
			free(lnk);
		}
		if(!strcmp("NICK", cmd) && !strcmp(usr, nick))
			strlcpy(nick, txt, sizeof nick);
	}
}

int
main(int argc, char *argv[]) {
	int i, c;
	struct timeval tv;
	const char *user = getenv("USER");
	fd_set rd;

	strlcpy(nick, user ? user : "unknown", sizeof nick);
	for(i = 1; i < argc; i++) {
		c = argv[i][1];
		if(argv[i][0] != '-' || argv[i][2])
			c = -1;
		switch(c) {
		case 'h':
			if(++i < argc) host = argv[i];
			break;
		case 'p':
			if(++i < argc) port = argv[i];
			break;
		case 'n':
			if(++i < argc) strlcpy(nick, argv[i], sizeof nick);
			break;
		case 'k':
			if(++i < argc) password = argv[i];
			break;
		case 'v':
			eprint("sic-"VERSION", © 2005-2012 Kris Maglione, Anselm R. Garbe, Nico Golde\n");
		default:
			eprint("usage: sic [-h host] [-p port] [-n nick] [-k keyword] [-v]\n");
		}
	}
	/* init */
	gotr_init();
	i = dial(host, port);
	srv = fdopen(i, "r+");
	/* login */
	if(password)
		sout("PASS %s", password);
	sout("NICK %s", nick);
	sout("USER %s localhost %s :%s", nick, host, nick);
	fflush(srv);
	setbuf(stdout, NULL);
	setbuf(srv, NULL);
	for(;;) { /* main loop */
		FD_ZERO(&rd);
		FD_SET(0, &rd);
		FD_SET(fileno(srv), &rd);
		tv.tv_sec = 120;
		tv.tv_usec = 0;
		i = select(fileno(srv) + 1, &rd, 0, 0, &tv);
		if(i < 0) {
			if(errno == EINTR)
				continue;
			eprint("sic: error on select():");
		}
		else if(i == 0) {
			if(time(NULL) - trespond >= 300)
				eprint("sic shutting down: parse timeout\n");
			sout("PING %s", host);
			continue;
		}
		if(FD_ISSET(fileno(srv), &rd)) {
			if(fgets(bufin, sizeof bufin, srv) == NULL)
				eprint("sic: remote host closed connection\n");
			parsesrv(bufin);
			trespond = time(NULL);
		}
		if(FD_ISSET(0, &rd)) {
			if(fgets(bufin, sizeof bufin, stdin) == NULL)
				eprint("sic: broken pipe\n");
			parsein(bufin);
		}
	}
	return 0;
}
