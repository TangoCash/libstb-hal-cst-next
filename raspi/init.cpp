/*
 * (C) 2010-2012 Stefan Seyfried
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * libstb-hal initialisation and input conversion routines
 * for the Raspberry Pi
 *
 */
#include <cstring>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>

#include <errno.h>

#include <set>
#include <map>
#include <thread_abstraction.h>

#include "init_lib.h"
#include "lt_debug.h"
#include "glfb.h"
#define lt_debug(args...) _lt_debug(TRIPLE_DEBUG_INIT, NULL, args)
#define lt_info(args...) _lt_info(TRIPLE_DEBUG_INIT, NULL, args)

static bool initialized = false;
GLFramebuffer *glfb = NULL;

typedef std::map<uint16_t, uint16_t> keymap_t;
static keymap_t kmap;

static void init_keymap(void)
{
	/* same as generic-pc/glfb.cpp */
	kmap[KEY_ENTER]	= KEY_OK;
	kmap[KEY_ESC]	= KEY_EXIT;
	kmap[KEY_E]	= KEY_EPG;
	kmap[KEY_I]	= KEY_INFO;
	kmap[KEY_M]	= KEY_MENU;
	kmap[KEY_F12]	= KEY_VOLUMEUP;		/* different than glfb, as we */
	kmap[KEY_F11]	= KEY_VOLUMEDOWN;	/* don't consider the keyboard */
	kmap[KEY_F10]	= KEY_MUTE;		/* layout... */
	kmap[KEY_H]	= KEY_HELP;
	kmap[KEY_P]	= KEY_POWER;
	kmap[KEY_F1]	= KEY_RED;
	kmap[KEY_F2]	= KEY_GREEN;
	kmap[KEY_F3]	= KEY_YELLOW;
	kmap[KEY_F4]	= KEY_BLUE;
	kmap[KEY_F5]	= KEY_WWW;
	kmap[KEY_F6]	= KEY_SUBTITLE;
	kmap[KEY_F7]	= KEY_MOVE;
	kmap[KEY_F8]	= KEY_SLEEP;
}

class Input: public Thread
{
	public:
		Input();
		~Input();
	private:
		void run();
		bool running;
};

Input::Input()
{
	Init();
	Thread::start();
}

Input::~Input()
{
	running = false;
	Thread::join();
}

static int dirfilter(const struct dirent *d)
{
	return !strncmp(d->d_name, "event", 5);
}

void Input::run()
{
	struct input_event in;
	int out_fd;
	struct dirent **namelist;
	int n;
	unsigned long bit = 0;
	char inputstr[] = "/dev/input/event9999999";
	std::set<int>in_fds;
	int fd_max = 0;
	fd_set rfds;
	hal_set_threadname("hal:input");
	init_keymap();
	unlink("/tmp/neutrino.input");
	mkfifo("/tmp/neutrino.input", 0600);
	out_fd = open("/tmp/neutrino.input", O_RDWR|O_CLOEXEC|O_NONBLOCK);
	if (out_fd < 0)
		lt_info("could not create /tmp/neutrino.input. good luck. error: %m\n");

	n = scandir("/dev/input", &namelist, dirfilter, NULL);
	if (n < 0)
		lt_info("no input devices /dev/input/eventX??\n");
	else
	{
		while (n--) {
			strcpy(inputstr + strlen("/dev/input/"), namelist[n]->d_name);
			free(namelist[n]);
			int fd = open(inputstr, O_RDWR|O_CLOEXEC|O_NONBLOCK);
			if (fd < 0) {
				lt_info("could not open %s:%m\n", inputstr);
				continue;
			}
			ioctl(fd, EVIOCGBIT(0, EV_MAX), &bit);
			if ((bit & (1 << EV_KEY)) == 0) {
				close(fd);
				continue;
			}
			lt_info("input dev: %s bit: 0x%08lx fd: %d\n", inputstr, bit, fd);
			in_fds.insert(fd);
			if (fd > fd_max)
				fd_max = fd;
		}
		free(namelist);
	}

	fd_max++;
	running = true;
	while (running) {
		FD_ZERO(&rfds);
		for (std::set<int>::iterator i = in_fds.begin(); i != in_fds.end(); ++i)
			FD_SET((*i), &rfds);

		/* timeout should not be necessary, but somehow cancel / cleanup did not
		 * work correctly with OpenThreads::Thread :-( */
		struct timeval timeout = { 0, 100000 }; /* 100ms */
		int ret = select(fd_max, &rfds, NULL, NULL, &timeout);
		if (ret == 0) /* timed out */
			continue;
		if (ret < 0) {
			lt_info("input: select returned %d (%m)\n", ret);
			continue;
		}

		for (std::set<int>::iterator i = in_fds.begin(); i != in_fds.end(); ++i) {
			if (!FD_ISSET((*i), &rfds))
				continue;

			ret = read(*i, &in, sizeof(in));
			if (ret != sizeof(in)) {
				if (errno == ENODEV) {
					close(*i);
					lt_info("input fd %d vanished?\n", *i);
					in_fds.erase(i);
				}
				continue;
			}
			if (in.type != EV_KEY)
				continue;
			keymap_t::const_iterator j = kmap.find(in.code);
			if (j != kmap.end())
				in.code = j->second;
			lt_debug("GLFB::%s:(fd %d) pushing 0x%x, value %d\n", __func__, *i, in.code, in.value);
			write(out_fd, &in, sizeof(in));
		}
	}
	for (std::set<int>::iterator i = in_fds.begin(); i != in_fds.end(); ++i)
		close(*i);
	in_fds.clear();
}

static Input *thread = NULL;

void init_td_api()
{
	if (!initialized)
		lt_debug_init();
	lt_info("%s begin, initialized=%d, debug=0x%02x\n", __func__, (int)initialized, debuglevel);
	if (! glfb) {
		int x = 1280, y = 720; /* default OSD FB resolution */
		/*
		 * export GLFB_RESOLUTION=720,576
		 * to restore old default behviour
		 */
		const char *tmp = getenv("GLFB_RESOLUTION");
		const char *p = NULL;
		if (tmp)
			p = strchr(tmp, ',');
		if (p) {
			x = atoi(tmp);
			y = atoi(p + 1);
		}
		lt_info("%s: setting Framebuffer size to %dx%d\n", __func__, x, y);
		if (!p)
			lt_info("%s: export GLFB_RESOLUTION=\"<w>,<h>\" to set another resolution\n", __func__);

		glfb = new GLFramebuffer(x, y); /* hard coded to PAL resolution for now */
	}
	if (! thread)
		thread = new Input();
	initialized = true;
}

void shutdown_td_api()
{
	lt_info("%s, initialized = %d\n", __func__, (int)initialized);
	if (glfb)
		delete glfb;
	if (thread)
		delete thread;
	initialized = false;
}
