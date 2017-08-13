/*
Copyright (C) 2017 by Azat Khasanshin <azat.khasanshin@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <fcntl.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <unistd.h>

#include <fstream>

#include "browser-app.hpp"

/* for signal handling */
int static_in_fd = 0;
BrowserApp *ba = NULL;
static void file_changed(int signum)
{
	struct inotify_event event;

	if (static_in_fd != 0 && ba != NULL) {
		(void)(read(static_in_fd, (void *) &event, sizeof(struct inotify_event))+1);
		ba->ReloadPage();
	}
}

BrowserApp::BrowserApp(char *shmname)
{
	if (shmname != NULL && strncmp(shmname, SHM_NAME, strlen(SHM_NAME)) == 0) {
		shm_name = strdup(shmname);

		/* init inotify */
		in_fd = inotify_init1(IN_NONBLOCK);
		fcntl(in_fd, F_SETFL, O_ASYNC);
		fcntl(in_fd, F_SETOWN, getpid());
		struct sigaction action;
		memset(&action, 0, sizeof(struct sigaction));
		action.sa_handler = file_changed;
		sigaction(SIGIO, &action, NULL);
		static_in_fd = in_fd;
		ba = this;

		InitSharedData();
	}
}

BrowserApp::~BrowserApp()
{
	UninitSharedData();
	if (shm_name)
		free(shm_name);
	if (in_fd)
		close(in_fd);
}

/* open shared memory and read initial data */
void BrowserApp::InitSharedData()
{
	fd = shm_open(shm_name, O_RDWR, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		printf("Browser: shared memory open failed\n");
		return;
	}
	data = (struct shared_data *) mmap(NULL, sizeof(struct shared_data),
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	if (data == MAP_FAILED) {
		printf("Browser: data mapping failed\n");
		return;
	}

	pthread_mutex_lock(&data->mutex);
	qid = data->qid;
	width = data->width;
	height = data->height;
	fps = data->fps;
	pthread_mutex_unlock(&data->mutex);

	data = (struct shared_data *) mmap(NULL, sizeof(struct shared_data) + MAX_DATA_SIZE,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		printf("Browser: data remapping faileda\n");
		return;
	}

}

void BrowserApp::UninitSharedData()
{
	if (data && data != MAP_FAILED) {
		munmap(data, sizeof(struct shared_data) + MAX_DATA_SIZE);
	}
}

/* message receiver thread */
static void *MessageThread(void *vptr)
{
	BrowserApp *ba = (BrowserApp *) vptr;
	size_t max_buf_size = MAX_MESSAGE_SIZE;
	ssize_t received;
	CefMouseEvent e;
	CefKeyEvent ke;

	struct generic_message msg;
	struct text_message *tmsg = (struct text_message *) &msg;
	struct mouse_click_message *cmsg = (struct mouse_click_message *) &msg;
	struct mouse_move_message *mmsg = (struct mouse_move_message *) &msg;
	struct mouse_wheel_message *wmsg = (struct mouse_wheel_message *) &msg;
	struct focus_message *fmsg = (struct focus_message *) &msg;
	struct key_message *kmsg = (struct key_message *) &msg;

	while (true) {
		received = msgrcv(ba->GetQueueId(), &msg, max_buf_size, 0, MSG_NOERROR);
		if (received != -1) {
			switch (msg.type) {
			case MESSAGE_TYPE_URL:
				ba->UrlChanged(tmsg->text);
				break;
			case MESSAGE_TYPE_SIZE:
				ba->SizeChanged();
				break;
			case MESSAGE_TYPE_RELOAD:
				ba->ReloadPage();
				break;
			case MESSAGE_TYPE_CSS:
				ba->CssChanged(tmsg->text);
				break;
			case MESSAGE_TYPE_MOUSE_CLICK:
				e.modifiers = cmsg->modifiers;
				e.x = cmsg->x;
				e.y = cmsg->y;
				ba->GetBrowser()->GetHost()->SendMouseClickEvent(e,
					(CefBrowserHost::MouseButtonType) cmsg->button_type,
					cmsg->mouse_up, cmsg->click_count);
				break;
			case MESSAGE_TYPE_MOUSE_MOVE:
				e.modifiers = mmsg->modifiers;
				e.x = mmsg->x;
				e.y = mmsg->y;
				ba->GetBrowser()->GetHost()->SendMouseMoveEvent(e, mmsg->mouse_leave);
				break;
			case MESSAGE_TYPE_MOUSE_WHEEL:
				e.modifiers = mmsg->modifiers;
				e.x = mmsg->x;
				e.y = mmsg->y;
				ba->GetBrowser()->GetHost()->SendMouseWheelEvent(e,
					wmsg->x_delta, wmsg->y_delta);
				break;
			case MESSAGE_TYPE_FOCUS:
				ba->GetBrowser()->GetHost()->SendFocusEvent(fmsg->focus);
				break;
			case MESSAGE_TYPE_KEY:
				/* I have no idea what is happening */
				ke.windows_key_code = kmsg->native_vkey;
				ke.native_key_code = kmsg->native_vkey;
				ke.modifiers = kmsg->modifiers;
				ke.type = kmsg->key_up ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;
				if (kmsg->chr != 0) {
					ke.character = kmsg->chr;
					if (!kmsg->key_up)
						ke.type = KEYEVENT_CHAR;
				}
				ba->GetBrowser()->GetHost()->SendKeyEvent(ke);
				break;
			case MESSAGE_TYPE_SCROLLBARS:
				ba->SetScrollbars((bool) msg.data[0]);
				break;
			}
		}
	}
}

void BrowserApp::SizeChanged()
{
	pthread_mutex_lock(&data->mutex);
	width = data->width;
	height = data->height;

	browser->GetHost()->WasResized();
	pthread_mutex_unlock(&data->mutex);
}

void BrowserApp::UrlChanged(const char *url)
{
	CefString cef_url;
	cef_url.FromString(std::string(url));
	browser->GetMainFrame()->LoadURL(cef_url);

	if (in_wd >= 0) {
		inotify_rm_watch(in_fd, in_wd);
		in_wd = -1;
	}

	if (strncmp(url, "file:///", strlen("file:///")) == 0) {
		in_wd = inotify_add_watch(in_fd, url+strlen("file://"), IN_MODIFY);
	}
}

void BrowserApp::CssChanged(const char *css_file)
{
	/* read file into string */
	std::ifstream t(css_file, std::ifstream::in);
	if (t.good()) {
		std::stringstream buffer;
		buffer << t.rdbuf();
		css = buffer.str();
	} else {
		css = "";
	}

	client->ChangeCss(css);
}

void BrowserApp::SetScrollbars(bool show)
{
	CefRefPtr<CefFrame> frame = browser->GetMainFrame();
	if (show) {
		frame->ExecuteJavaScript(std::string("document.documentElement.style.overflow = 'auto';"),
			frame->GetURL(), 0);
	} else {
		frame->ExecuteJavaScript(std::string("document.documentElement.style.overflow = 'hidden';"),
			frame->GetURL(), 0);
	}
	client->SetScrollbars(show);
}

void BrowserApp::ReloadPage()
{
	browser->ReloadIgnoreCache();
}

/* browser instance created in this callback */
void BrowserApp::OnContextInitialized()
{
	if (!shm_name)
		return;

	CefWindowInfo info;
	info.transparent_painting_enabled = true;
	info.width = width;
	info.height = height;
	info.windowless_rendering_enabled = true;

	CefBrowserSettings settings;
	settings.windowless_frame_rate = fps;

	CefRefPtr<BrowserClient> client(new BrowserClient(data, css));
	this->client = client;

	browser = CefBrowserHost::CreateBrowserSync(info, client.get(), "http://google.com", settings, NULL);

	pthread_create(&message_thread, NULL, MessageThread, this);
}
