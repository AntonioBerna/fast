#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define API_FALLBACK "api.fast.com/netflix/speedtest/v2"
#define MAX_TARGETS 5
#define WIN_SAMPLES 128
#define HIST_SAMPLES 12
#define BIG_RANGE "0-1073741823"
#define UPLOAD_SIZE (8 * 1024 * 1024)

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} buffer_t;

typedef struct {
	bool upload;
	bool json;
	bool verbose;
} options_t;

typedef struct {
	char api[128];
	char token[128];
	char ip[64];
	char user_location[128];
	char urls[MAX_TARGETS][1024];
	char servers[MAX_TARGETS][128];
	int target_count;
} fast_info_t;

typedef struct {
	bool tty;
	bool raw;
	int lines;
	FILE *stream;
	struct termios saved;
} ui_t;

typedef struct {
	double cur_download;
	double cur_upload;
	double download;
	double upload;
	double unloaded_latency;
	double loaded_latency;
	size_t downloaded;
	size_t uploaded;
} result_t;

typedef struct {
	const options_t *options;
	const fast_info_t *fast;
	ui_t *ui;
	result_t *result;
	bool upload;
	double start;
	double min_seconds;
	double max_seconds;
	double last_draw;
	double last_hist;
	double win_time[WIN_SAMPLES];
	size_t win_bytes[WIN_SAMPLES];
	int win_start;
	int win_count;
	double hist[HIST_SAMPLES];
	int hist_count;
	curl_off_t last_now;
	size_t total_bytes;
	bool done;
} meter_t;

typedef struct {
	const char *url;
	volatile sig_atomic_t active;
	double samples[16];
	int count;
} ping_thread_t;

static volatile sig_atomic_t g_stop = 0;

static bool needs_latency(const options_t *options) {
	return options->json || options->verbose;
}

static void on_signal(int signum) {
	(void) signum;
	g_stop = 1;
}

static double now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1000000000.0;
}

static void sleep_ms(long ms) {
	struct timespec req = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
	while (nanosleep(&req, &req) == -1 && errno == EINTR) {
	}
}

static void buf_free(buffer_t *buffer) {
	free(buffer->data);
	buffer->data = NULL;
	buffer->len = 0;
	buffer->cap = 0;
}

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
	buffer_t *buffer = userdata;
	size_t bytes = size * nmemb;
	size_t need = buffer->len + bytes + 1;

	if (need > buffer->cap) {
		size_t cap = buffer->cap ? buffer->cap * 2 : 4096;
		while (cap < need) {
			cap *= 2;
		}
		char *data = realloc(buffer->data, cap);
		if (!data) {
			return 0;
		}
		buffer->data = data;
		buffer->cap = cap;
	}

	memcpy(buffer->data + buffer->len, ptr, bytes);
	buffer->len += bytes;
	buffer->data[buffer->len] = 0;
	return bytes;
}

static size_t sink_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
	(void) ptr;
	(void) userdata;
	return size * nmemb;
}

static size_t copy_slice(char *out, size_t out_size, const char *src, size_t len) {
	if (!out_size) {
		return 0;
	}
	if (len >= out_size) {
		len = out_size - 1;
	}
	if (len) {
		memcpy(out, src, len);
	}
	out[len] = 0;
	return len;
}

static size_t append_text(char *out, size_t out_size, const char *src) {
	size_t used = strnlen(out, out_size);
	if (used == out_size) {
		out[out_size - 1] = 0;
		return out_size - 1;
	}
	return used + copy_slice(out + used, out_size - used, src, strlen(src));
}

static char *http_text(const char *url, long timeout_ms) {
	buffer_t buffer = { 0 };
	CURL *curl = curl_easy_init();
	long status = 0;
	if (!curl) return NULL;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "fast-c/1.0");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (code != CURLE_OK || status < 200 || status >= 300) {
		buf_free(&buffer);
		return NULL;
	}

	return buffer.data ? buffer.data : strdup("");
}

static bool copy_between(const char *src, const char *begin, const char *end, char *out, size_t out_size) {
	const char *from = strstr(src, begin);
    if (!from) return false;
	from += strlen(begin);
    const char *to = strstr(from, end);
	if (!to) return false;
	size_t len = (size_t) (to - from);
	copy_slice(out, out_size, from, len);
	return true;
}

static bool json_string(const char *src, const char *key, char *out, size_t out_size) {
	char needle[64];
	snprintf(needle, sizeof(needle), "\"%s\":\"", key);
	const char *p = strstr(src, needle);
	if (!p) return false;
	p += strlen(needle);
	size_t n = 0;
	while (*p && *p != '"' && n + 1 < out_size) {
		if (*p == '\\' && p[1]) {
			p += 1;
		}
		out[n++] = *p++;
	}
	out[n] = 0;
	return true;
}

static void format_location(char *out, size_t out_size, const char *city, const char *country) {
	if (*city && *country) {
		snprintf(out, out_size, "%s, %s", city, country);
	} else if (*city) {
		snprintf(out, out_size, "%s", city);
	} else {
		snprintf(out, out_size, "%s", country);
	}
}

static int cmp_double(const void *left, const void *right) {
	const double a = *(const double *) left;
	const double b = *(const double *) right;
	return (a > b) - (a < b);
}

static double percentile(const double *values, int count, double pct) {
	if (count <= 0) {
		return -1.0;
	}
	double tmp[32];
	if (count > (int) (sizeof(tmp) / sizeof(tmp[0]))) {
		count = (int) (sizeof(tmp) / sizeof(tmp[0]));
	}
	for (int i = 0; i < count; i++) {
		tmp[i] = values[i];
	}
	qsort(tmp, (size_t) count, sizeof(tmp[0]), cmp_double);
	return tmp[(int) ((count - 1) * pct)];
}

static bool load_fast_info(fast_info_t *fast) {
	char js_path[256];
	char api_url[512];
	char city[64] = "";
	char country[32] = "";
	char *page = NULL;
	char *bundle = NULL;
	char *json = NULL;
	const char *script = NULL;
	const char *client = NULL;
	const char *targets = NULL;
	bool ok = false;

	memset(fast, 0, sizeof(*fast));
	page = http_text("https://fast.com", 15000L);
	if (!page) goto cleanup;
	script = strstr(page, "<script src=\"/app-");
	if (!script || !copy_between(script, "<script src=\"", "\"", js_path, sizeof(js_path))) goto cleanup;

	char full_js[512];
	snprintf(full_js, sizeof(full_js), "https://fast.com%s", js_path);
	bundle = http_text(full_js, 20000L);
	if (!bundle) goto cleanup;

	if (!copy_between(bundle, "token:\"", "\"", fast->token, sizeof(fast->token))) {
		goto cleanup;
	}
	if (!copy_between(bundle, "apiEndpoint=\"", "\"", fast->api, sizeof(fast->api))) {
		snprintf(fast->api, sizeof(fast->api), "%s", API_FALLBACK);
	}

	snprintf(api_url, sizeof(api_url), "https://%s?https=true&token=%s&urlCount=%d", fast->api, fast->token, MAX_TARGETS);
	json = http_text(api_url, 15000L);
	if (!json) goto cleanup;

	client = strstr(json, "\"client\":{");
	targets = strstr(json, "\"targets\":[");
	if (!client || !targets) goto cleanup;

	json_string(client, "ip", fast->ip, sizeof(fast->ip));
	const char *client_loc = strstr(client, "\"location\":{");
	if (client_loc) {
		json_string(client_loc, "city", city, sizeof(city));
		json_string(client_loc, "country", country, sizeof(country));
	}
	format_location(fast->user_location, sizeof(fast->user_location), city, country);

	fast->target_count = 0;
	const char *cursor = targets;
	while (fast->target_count < MAX_TARGETS) {
		const char *url_key = strstr(cursor, "\"url\":\"");
		if (!url_key) {
			break;
		}
		url_key += strlen("\"url\":\"");
		const char *url_end = strchr(url_key, '"');
		if (!url_end) {
			break;
		}
		size_t url_len = (size_t) (url_end - url_key);
		copy_slice(fast->urls[fast->target_count], sizeof(fast->urls[0]), url_key, url_len);

		char server_city[64] = "";
		char server_country[32] = "";
		const char *next_url = strstr(url_end, "\"url\":\"");
		const char *loc = strstr(url_end, "\"location\":{");
		if (loc && (!next_url || loc < next_url)) {
			json_string(loc, "city", server_city, sizeof(server_city));
			json_string(loc, "country", server_country, sizeof(server_country));
		}
		format_location(fast->servers[fast->target_count], sizeof(fast->servers[fast->target_count]), server_city, server_country);
		fast->target_count += 1;
		cursor = url_end + 1;
	}

	ok = fast->target_count > 0;

cleanup:
	free(json);
	free(bundle);
	free(page);
	return ok;
}

static void join_servers(const fast_info_t *fast, char *out, size_t out_size) {
	out[0] = 0;
	for (int i = 0; i < fast->target_count; i++) {
		if (i) {
			append_text(out, out_size, " | ");
		}
		append_text(out, out_size, fast->servers[i]);
	}
}

static void ui_start(ui_t *ui, FILE *stream) {
	memset(ui, 0, sizeof(*ui));
	ui->stream = stream;
	ui->tty = stream && isatty(fileno(stream));
	if (!ui->tty) {
		return;
	}
	if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &ui->saved) == 0) {
		struct termios raw = ui->saved;
		raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		ui->raw = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
	}
	fputs("\033[?25l", ui->stream);
	fflush(ui->stream);
}

static void ui_clear(ui_t *ui) {
	if (!ui->tty || !ui->lines) {
		return;
	}
	if (ui->lines > 1) {
		fprintf(ui->stream, "\033[%dA", ui->lines - 1);
	}
	for (int i = 0; i < ui->lines; i++) {
		fputs("\r\033[2K", ui->stream);
		if (i + 1 < ui->lines) {
			fputs("\033[1B", ui->stream);
		}
	}
	if (ui->lines > 1) {
		fprintf(ui->stream, "\033[%dA", ui->lines - 1);
	}
}

static void ui_stop(ui_t *ui, bool newline) {
	if (!ui->tty) {
		return;
	}
	if (ui->raw) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &ui->saved);
	}
	fputs("\033[?25h", ui->stream);
	if (newline) {
		fputc('\n', ui->stream);
	}
	fflush(ui->stream);
	ui->lines = 0;
}

static void ui_render(ui_t *ui, const options_t *options, const fast_info_t *fast, const result_t *result, bool final) {
	if (!ui->tty) {
		return;
	}

	char servers[512];
	char main_line[256];
	char latency[256];
	char client[256];
	char server[576];
	static const char *spinner[] = {"◜", "◠", "◝", "◞", "◡", "◟"};
	const char *frame = spinner[(int) (now_seconds() * 12.0) % (int) (sizeof(spinner) / sizeof(spinner[0]))];
	double down = final ? result->download : result->cur_download;
	double up = final ? result->upload : result->cur_upload;

	join_servers(fast, servers, sizeof(servers));
	if (options->upload) {
		snprintf(main_line, sizeof(main_line), "%s %.2f Mbps down / %.2f Mbps up", final ? " " : frame, down, up);
	} else {
		snprintf(main_line, sizeof(main_line), "%s %.2f Mbps down", final ? " " : frame, down);
	}

	ui_clear(ui);
	fputs("\r", ui->stream);
	fputs(main_line, ui->stream);
	ui->lines = 1;

	if (options->verbose) {
		if (result->unloaded_latency >= 0.0 && result->loaded_latency >= 0.0) {
			snprintf(latency, sizeof(latency), "Latency: %.0f ms (unloaded) / %.0f ms (loaded)", result->unloaded_latency, result->loaded_latency);
		} else if (result->unloaded_latency >= 0.0) {
			snprintf(latency, sizeof(latency), "Latency: %.0f ms (unloaded)", result->unloaded_latency);
		} else {
			snprintf(latency, sizeof(latency), "Latency: measuring...");
		}
		snprintf(client, sizeof(client), "Client: %s%s%s", fast->user_location, *fast->user_location && *fast->ip ? " | " : "", fast->ip);
		snprintf(server, sizeof(server), "Server: %s", *servers ? servers : "detecting...");
		fprintf(ui->stream, "\n%s\n%s\n%s", latency, client, server);
		ui->lines = 4;
	}

	fflush(ui->stream);
}

static void meter_push(meter_t *meter, size_t bytes, double t) {
	if (meter->win_count == WIN_SAMPLES) {
		meter->win_start = (meter->win_start + 1) % WIN_SAMPLES;
		meter->win_count -= 1;
	}
	int slot = (meter->win_start + meter->win_count) % WIN_SAMPLES;
	meter->win_time[slot] = t;
	meter->win_bytes[slot] = bytes;
	meter->win_count += 1;
}

static double meter_rate(const meter_t *meter, double t) {
	size_t bytes = 0;
	double first = 0.0;
	bool seen = false;
	for (int i = 0; i < meter->win_count; i++) {
		int slot = (meter->win_start + i) % WIN_SAMPLES;
		if (t - meter->win_time[slot] > 0.8) {
			continue;
		}
		if (!seen) {
			first = meter->win_time[slot];
			seen = true;
		}
		bytes += meter->win_bytes[slot];
	}
    if (!seen) return 0.0;

    double dt = t - first;
	if (dt < 0.05) dt = 0.05;
	return (double) bytes * 8.0 / dt / 1000000.0;
}

static void meter_hist(meter_t *meter, double value) {
	if (meter->hist_count < HIST_SAMPLES) {
		meter->hist[meter->hist_count++] = value;
	} else {
		memmove(meter->hist, meter->hist + 1, sizeof(meter->hist[0]) * (HIST_SAMPLES - 1));
		meter->hist[HIST_SAMPLES - 1] = value;
	}
}

static bool meter_stable(const meter_t *meter) {
	if (meter->hist_count < 6) {
		return false;
	}
	double min = meter->hist[meter->hist_count - 6];
	double max = min;
	for (int i = meter->hist_count - 6; i < meter->hist_count; i++) {
		if (meter->hist[i] < min) {
			min = meter->hist[i];
		}
		if (meter->hist[i] > max) {
			max = meter->hist[i];
		}
	}
	return max > 0.0 && ((max - min) / max) <= 0.08;
}

static double meter_final(const meter_t *meter) {
	if (meter->hist_count > 0) {
		double sum = 0.0;
		int take = meter->hist_count < 6 ? meter->hist_count : 6;
		for (int i = meter->hist_count - take; i < meter->hist_count; i++) {
			sum += meter->hist[i];
		}
		return sum / take;
	}
	double dt = now_seconds() - meter->start;
	return dt > 0.0 ? (double) meter->total_bytes * 8.0 / dt / 1000000.0 : 0.0;
}

static int meter_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
	meter_t *meter = userdata;
	(void) dltotal;
	(void) ultotal;
	double t = now_seconds();
	if (g_stop) {
		meter->done = true;
		return 1;
	}

	curl_off_t now = meter->upload ? ulnow : dlnow;
	if (now < meter->last_now) meter->last_now = 0;
	if (now > meter->last_now) {
		size_t delta = (size_t) (now - meter->last_now);
		meter->last_now = now;
		meter->total_bytes += delta;
		meter_push(meter, delta, t);
		double rate = meter_rate(meter, t);
		if (meter->upload) {
			meter->result->uploaded = meter->total_bytes;
			meter->result->cur_upload = rate;
		} else {
			meter->result->downloaded = meter->total_bytes;
			meter->result->cur_download = rate;
		}
		if (rate > 0.0 && t - meter->last_hist >= 0.2) {
			meter_hist(meter, rate);
			meter->last_hist = t;
		}
		if (((t - meter->start) >= meter->min_seconds && meter_stable(meter)) || (t - meter->start) >= meter->max_seconds) {
			meter->done = true;
			return 1;
		}
	}
	if (meter->ui->tty && t - meter->last_draw >= 0.05) {
		ui_render(meter->ui, meter->options, meter->fast, meter->result, false);
		meter->last_draw = t;
	}
	return 0;
}

static void curl_measure_opts(CURL *curl, meter_t *meter) {
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, meter_progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, meter);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "fast-c/1.0");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

static double ping_once(const char *url) {
	CURL *curl = curl_easy_init();
	if (!curl) return -1.0;

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "fast-c/1.0");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	CURLcode code = curl_easy_perform(curl);
	double time = -1.0;
	if (code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &time);
	curl_easy_cleanup(curl);
	return time < 0.0 ? -1.0 : time * 1000.0;
}

static void *ping_thread(void *userdata) {
	ping_thread_t *thread = userdata;
	while (thread->active && !g_stop) {
		double ms = ping_once(thread->url);
		if (ms >= 0.0 && thread->count < (int) (sizeof(thread->samples) / sizeof(thread->samples[0]))) {
			thread->samples[thread->count++] = ms;
		}
		sleep_ms(1000);
	}
	return NULL;
}

static bool run_phase(const fast_info_t *fast, const options_t *options, ui_t *ui, result_t *result, bool upload) {
	meter_t meter = {
		.options = options,
		.fast = fast,
		.ui = ui,
		.result = result,
		.upload = upload,
		.start = now_seconds(),
		.min_seconds = upload ? 4.0 : 5.0,
		.max_seconds = upload ? 12.0 : 15.0,
	};

	char *payload = NULL;
	struct curl_slist *headers = NULL;
	ping_thread_t ping = { 0 };
	pthread_t ping_tid;
	bool have_ping_thread = false;
	bool ok = false;
	bool collect_latency = needs_latency(options);

	if (upload) {
		payload = calloc(1, UPLOAD_SIZE);
		if (!payload) goto cleanup;
		headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
		if (!headers) goto cleanup;
		if (collect_latency) {
			ping.url = fast->urls[0];
			ping.active = 1;
			have_ping_thread = pthread_create(&ping_tid, NULL, ping_thread, &ping) == 0;
		}
	}

	for (int turn = 0; !g_stop && !meter.done; turn++) {
		CURL *curl = curl_easy_init();
		if (!curl) {
			break;
		}
		meter.last_now = 0;
		curl_measure_opts(curl, &meter);
		curl_easy_setopt(curl, CURLOPT_URL, fast->urls[turn % (fast->target_count < 2 ? fast->target_count : 2)]);
		if (upload) {
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) UPLOAD_SIZE);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		} else {
			curl_easy_setopt(curl, CURLOPT_RANGE, BIG_RANGE);
		}
		CURLcode code = curl_easy_perform(curl);
		curl_easy_cleanup(curl);
		if (code != CURLE_OK && code != CURLE_ABORTED_BY_CALLBACK && meter.total_bytes == 0) break;
		if ((now_seconds() - meter.start) >= meter.max_seconds) break;
	}
	ok = meter.total_bytes > 0 && !g_stop;

	cleanup:
	if (have_ping_thread) {
		ping.active = 0;
		pthread_join(ping_tid, NULL);
		result->loaded_latency = percentile(ping.samples, ping.count, 0.75);
	}

	free(payload);
	curl_slist_free_all(headers);
	if (upload) {
		result->upload = meter_final(&meter);
		result->uploaded = meter.total_bytes;
	} else {
		result->download = meter_final(&meter);
		result->downloaded = meter.total_bytes;
	}
	return ok;
}

static void print_help(FILE *stream) {
	fputs(
		"Usage\n"
		"  fast\n"
		"  fast > file\n"
		"\n"
		"Choose at most one of --upload, --json, or --verbose.\n"
		"\n"
		"Options\n"
		"  --upload, -u   Measure upload speed in addition to download speed\n"
		"  --json         JSON output\n"
		"  --verbose      Include latency and server location information\n"
		"  --help, -h     Show this help\n",
		stream
	);
}

static bool parse_options(int argc, char **argv, options_t *options) {
	static const struct option long_options[] = {
		{"upload", no_argument, NULL, 'u'},
		{"json", no_argument, NULL, 'j'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};

	memset(options, 0, sizeof(*options));
	bool show_help = false;
	int selected = 0;
	for (;;) {
		int idx = 0;
		int c = getopt_long(argc, argv, "ujvh", long_options, &idx);
		if (c == -1) {
			break;
		}
		switch (c) {
			case 'u': options->upload = true; selected += 1; break;
			case 'j': options->json = true; selected += 1; break;
			case 'v': options->verbose = true; options->upload = true; selected += 1; break;
			case 'h': show_help = true; selected += 1; break;
			default: print_help(stderr); return false;
		}
	}
	if (optind != argc) {
		fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
		print_help(stderr);
		return false;
	}
	if (selected > 1) {
		fputs("Select only one option at a time.\n", stderr);
		print_help(stderr);
		return false;
	}
	if (show_help) {
		print_help(stdout);
		exit(0);
	}
	return true;
}

static void print_json_string(const char *s) {
	putchar('"');
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') {
			putchar('\\');
			putchar(*s);
		} else if (*s == '\n') {
			fputs("\\n", stdout);
		} else {
			putchar(*s);
		}
	}
	putchar('"');
}

static void text_output(const fast_info_t *fast, const options_t *options, const result_t *result) {
	printf("%.2f Mbps\n", result->download);
	if (options->upload) printf("%.2f Mbps\n", result->upload);
	if (options->verbose) {
		char servers[512];
		join_servers(fast, servers, sizeof(servers));
		printf("\nLatency: %.0f ms (unloaded)", result->unloaded_latency >= 0.0 ? result->unloaded_latency : 0.0);
		if (result->loaded_latency >= 0.0) {
			printf(" / %.0f ms (loaded)", result->loaded_latency);
		}
		printf("\nClient: %s%s%s\n", fast->user_location, *fast->user_location && *fast->ip ? " | " : "", fast->ip);
		printf("Server: %s\n", servers);
	}
}

static void json_output(const fast_info_t *fast, const options_t *options, const result_t *result) {
	printf("{\n  \"downloadSpeed\": %.6f,\n", result->download);
	if (options->upload) printf("  \"uploadSpeed\": %.6f,\n", result->upload);
	printf("  \"downloadUnit\": \"Mbps\",\n");
	if (options->upload) printf("  \"uploadUnit\": \"Mbps\",\n");
	printf("  \"downloaded\": %.6f,\n", (double) result->downloaded / 1000000.0);
	if (options->upload) printf("  \"uploaded\": %.6f,\n", (double) result->uploaded / 1000000.0);
	printf("  \"latency\": %.0f,\n", result->unloaded_latency >= 0.0 ? result->unloaded_latency : 0.0);
	printf("  \"bufferBloat\": %.0f,\n", result->loaded_latency >= 0.0 ? result->loaded_latency : 0.0);
	printf("  \"userLocation\": ");
	print_json_string(fast->user_location);
	printf(",\n  \"serverLocations\": [");
	for (int i = 0; i < fast->target_count; i++) {
		if (i) {
			printf(", ");
		}
		print_json_string(fast->servers[i]);
	}
	printf("],\n  \"userIp\": ");
	print_json_string(fast->ip);
	printf("\n}\n");
}

int main(int argc, char **argv) {
	options_t options;
	fast_info_t fast = {0};
	ui_t ui;
	result_t result = {.unloaded_latency = -1.0, .loaded_latency = -1.0};
	double ping_samples[6];
	int ping_count = 0;
	bool ok = false;

	if (!parse_options(argc, argv, &options)) return 1;
    
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
		fputs("Failed to initialize libcurl\n", stderr);
		return 1;
	}
	if (!load_fast_info(&fast)) {
		fputs("Failed to load fast.com configuration\n", stderr);
		curl_global_cleanup();
		return 1;
	}

	ui_start(&ui, options.json ? stderr : stdout);
	if (needs_latency(&options)) {
		for (int i = 0; i < 6 && !g_stop; i++) {
			double ms = ping_once(fast.urls[0]);
			if (ms >= 0.0) {
				ping_samples[ping_count++] = ms;
			}
			sleep_ms(100);
		}
		result.unloaded_latency = percentile(ping_samples, ping_count, 0.10);
	}

	ok = run_phase(&fast, &options, &ui, &result, false);
	if (ok && options.upload) {
		ok = run_phase(&fast, &options, &ui, &result, true);
	}

	if (ui.tty) {
		ui_render(&ui, &options, &fast, &result, true);
		ui_stop(&ui, true);
	}
	if (options.json) {
		json_output(&fast, &options, &result);
	} else if (!ui.tty) {
		text_output(&fast, &options, &result);
	}

	curl_global_cleanup();
	return g_stop ? 130 : (ok ? 0 : 1);
}
