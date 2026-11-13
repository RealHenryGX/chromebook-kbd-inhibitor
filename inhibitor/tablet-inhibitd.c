#define _GNU_SOURCE

#include <libevdev/libevdev.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INPUT_DIR "/dev/input"
#define SYS_INPUT_DIR "/sys/class/input"
#define DEFAULT_INTERNAL_KEYBOARD "AT Translated Set 2 keyboard"
#define DEBOUNCE_MS 120
#define RETRY_MS 2000
#define RECONNECT_MS 1500
#define KEYBOARD_SCAN_MS 5000

struct keyboard {
    char *inhibited_path;
};

struct keyboard_list {
    struct keyboard *items;
    size_t count;
};

static volatile sig_atomic_t stopping;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool read_device_name(const char *event_name, char *name, size_t size)
{
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s/device/name",
                     SYS_INPUT_DIR, event_name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;

    FILE *file = fopen(path, "r");
    if (!file)
        return false;

    bool ok = fgets(name, (int)size, file) != NULL;
    int saved_errno = errno;
    if (fclose(file) != 0 && ok)
        ok = false;
    errno = saved_errno;

    if (ok)
        name[strcspn(name, "\r\n")] = '\0';
    return ok;
}

static bool keyboard_path_exists(const char *event_name, char *path, size_t size)
{
    char device_path[512];
    char resolved[PATH_MAX];
    int n = snprintf(device_path, sizeof(device_path), "%s/%s/device",
                     SYS_INPUT_DIR, event_name);
    if (n < 0 || (size_t)n >= sizeof(device_path) ||
        !realpath(device_path, resolved))
        return false;

    n = snprintf(path, size, "%s/inhibited", resolved);
    if (n < 0 || (size_t)n >= size)
        return false;
    return access(path, F_OK) == 0;
}

static bool path_is_listed(const struct keyboard_list *list, const char *path)
{
    for (size_t i = 0; i < list->count; ++i) {
        if (strcmp(list->items[i].inhibited_path, path) == 0)
            return true;
    }
    return false;
}

static int add_internal_keyboards(struct keyboard_list *list,
                                  const char *name_selector,
                                  bool announce)
{
    DIR *dir = opendir(INPUT_DIR);
    if (!dir) {
        fprintf(stderr, "cannot open %s: %s\n", INPUT_DIR, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        char name[256];
        if (!read_device_name(entry->d_name, name, sizeof(name)) ||
            strcmp(name, name_selector) != 0)
            continue;

        char event_path[512];
        int n = snprintf(event_path, sizeof(event_path), "%s/%s",
                         INPUT_DIR, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(event_path))
            continue;

        int fd = open(event_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct libevdev *dev = NULL;
        int rc = libevdev_new_from_fd(fd, &dev);
        bool is_keyboard = rc == 0 &&
            libevdev_has_event_type(dev, EV_KEY) &&
            libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
            libevdev_has_event_code(dev, EV_KEY, KEY_SPACE);
        if (dev)
            libevdev_free(dev);
        close(fd);
        if (!is_keyboard)
            continue;

        char inhibit_path[PATH_MAX];
        if (!keyboard_path_exists(entry->d_name, inhibit_path,
                                  sizeof(inhibit_path)))
            continue;
        if (path_is_listed(list, inhibit_path))
            continue;

        struct keyboard *items = realloc(
            list->items, (list->count + 1) * sizeof(*list->items));
        if (!items) {
            fprintf(stderr, "out of memory while listing keyboards\n");
            closedir(dir);
            return -1;
        }
        list->items = items;
        list->items[list->count].inhibited_path = strdup(inhibit_path);
        if (!list->items[list->count].inhibited_path) {
            fprintf(stderr, "out of memory while listing keyboards\n");
            closedir(dir);
            return -1;
        }
        ++list->count;
        if (announce)
            fprintf(stderr, "targeting internal keyboard: %s (%s)\n", name,
                    inhibit_path);
    }

    closedir(dir);
    return 0;
}

static void free_keyboard_list(struct keyboard_list *list)
{
    for (size_t i = 0; i < list->count; ++i)
        free(list->items[i].inhibited_path);
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

static int refresh_internal_keyboards(struct keyboard_list *list,
                                      const char *name_selector)
{
    struct keyboard_list refreshed = {0};
    if (add_internal_keyboards(&refreshed, name_selector, false) < 0 ||
        refreshed.count == 0) {
        free_keyboard_list(&refreshed);
        return -1;
    }

    bool unchanged = list->count == refreshed.count;
    if (unchanged) {
        for (size_t i = 0; i < refreshed.count; ++i) {
            if (!path_is_listed(list, refreshed.items[i].inhibited_path)) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged) {
        free_keyboard_list(&refreshed);
        return 0;
    }

    free_keyboard_list(list);
    *list = refreshed;
    return 1;
}

static bool set_one_inhibited(const char *path, bool inhibited)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }

    const char value = inhibited ? '1' : '0';
    ssize_t written;
    do {
        written = write(fd, &value, 1);
    } while (written < 0 && errno == EINTR);

    int saved_errno = errno;
    if (close(fd) < 0 && written == 1) {
        fprintf(stderr, "cannot close %s: %s\n", path, strerror(errno));
        return false;
    }
    errno = saved_errno;
    if (written != 1) {
        fprintf(stderr, "cannot write %s: %s\n", path,
                written < 0 ? strerror(errno) : "short write");
        return false;
    }
    return true;
}

static bool set_inhibited(const struct keyboard_list *list, bool inhibited)
{
    bool success = true;
    for (size_t i = 0; i < list->count; ++i) {
        if (!set_one_inhibited(list->items[i].inhibited_path, inhibited))
            success = false;
    }
    return success;
}

static struct libevdev *find_tablet_switch(int *fd_out)
{
    DIR *dir = opendir(INPUT_DIR);
    if (!dir) {
        fprintf(stderr, "cannot open %s: %s\n", INPUT_DIR, strerror(errno));
        return NULL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        char path[512];
        int n = snprintf(path, sizeof(path), "%s/%s", INPUT_DIR,
                         entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct libevdev *dev = NULL;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc == 0 && libevdev_has_event_type(dev, EV_SW) &&
            libevdev_has_event_code(dev, EV_SW, SW_TABLET_MODE)) {
            *fd_out = fd;
            closedir(dir);
            return dev;
        }

        if (dev)
            libevdev_free(dev);
        close(fd);
    }

    closedir(dir);
    return NULL;
}

static bool apply_state(const struct keyboard_list *keyboards, bool state)
{
    fprintf(stderr, "tablet mode %s: internal keyboard %s\n",
            state ? "on" : "off", state ? "inhibited" : "enabled");
    return set_inhibited(keyboards, state);
}

static void update_candidate(const struct input_event *event,
                             bool *candidate_known,
                             bool *candidate_state,
                             long long *candidate_since,
                             long long *retry_at)
{
    if (event->type != EV_SW || event->code != SW_TABLET_MODE)
        return;

    bool next_state = event->value != 0;
    if (!*candidate_known || next_state != *candidate_state) {
        *candidate_state = next_state;
        *candidate_known = true;
        *candidate_since = monotonic_ms();
        *retry_at = 0;
    }
}

int main(void)
{
    const char *selector = getenv("TABLET_INHIBIT_KEYBOARD_NAME");
    if (!selector || !*selector)
        selector = DEFAULT_INTERNAL_KEYBOARD;

    struct keyboard_list keyboards = {0};
    if (add_internal_keyboards(&keyboards, selector, true) < 0) {
        fprintf(stderr, "initial keyboard scan failed; will retry\n");
        free_keyboard_list(&keyboards);
    }
    if (keyboards.count == 0) {
        fprintf(stderr,
                "waiting for a built-in keyboard with an inhibited attribute named '%s'\n"
                "set TABLET_INHIBIT_KEYBOARD_NAME to the exact built-in keyboard name\n",
                selector);
    }

    struct sigaction action = {0};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        fprintf(stderr, "cannot install signal handlers: %s\n", strerror(errno));
        free_keyboard_list(&keyboards);
        return 1;
    }

    struct libevdev *tablet = NULL;
    int tablet_fd = -1;
    bool applied_known = false;
    bool applied_state = false;
    bool candidate_known = false;
    bool candidate_state = false;
    long long candidate_since = 0;
    long long retry_at = 0;
    long long next_keyboard_scan = monotonic_ms() + KEYBOARD_SCAN_MS;

    while (!stopping) {
        if (!tablet) {
            tablet = find_tablet_switch(&tablet_fd);
            if (!tablet) {
                fprintf(stderr, "tablet-mode switch unavailable; retrying\n");
                struct timespec delay = {.tv_sec = RECONNECT_MS / 1000,
                                         .tv_nsec = (RECONNECT_MS % 1000) * 1000000L};
                nanosleep(&delay, NULL);
                continue;
            }

            int state = libevdev_get_event_value(tablet, EV_SW, SW_TABLET_MODE);
            if (state < 0) {
                fprintf(stderr, "cannot read initial tablet-mode state: %s\n",
                        strerror(-state));
                libevdev_free(tablet);
                close(tablet_fd);
                tablet = NULL;
                tablet_fd = -1;
                struct timespec delay = {.tv_sec = RECONNECT_MS / 1000,
                                         .tv_nsec = (RECONNECT_MS % 1000) * 1000000L};
                nanosleep(&delay, NULL);
                continue;
            }
            candidate_state = state != 0;
            candidate_known = true;
            candidate_since = monotonic_ms() - DEBOUNCE_MS;
            applied_known = false;
            fprintf(stderr, "tablet-mode switch connected, current state: %s\n",
                    candidate_state ? "on" : "off");
        }

        long long now = monotonic_ms();
        if (now >= next_keyboard_scan) {
            next_keyboard_scan = now + KEYBOARD_SCAN_MS;
            int refreshed = refresh_internal_keyboards(&keyboards, selector);
            if (refreshed > 0) {
                fprintf(stderr, "built-in keyboard device path changed; reapplying mode\n");
                applied_known = false;
                candidate_since = now - DEBOUNCE_MS;
                retry_at = 0;
            }
        }
        bool waiting_to_retry = !applied_known && retry_at != 0 &&
                                now < retry_at;
        if (keyboards.count > 0 && candidate_known &&
            (!applied_known || candidate_state != applied_state) &&
            !waiting_to_retry && now - candidate_since >= DEBOUNCE_MS) {
            bool success = apply_state(&keyboards, candidate_state);
            if (success) {
                applied_state = candidate_state;
                applied_known = true;
                retry_at = 0;
            } else {
                applied_known = false;
                retry_at = now + RETRY_MS;
                refresh_internal_keyboards(&keyboards, selector);
            }
        }

        struct pollfd pfd = {.fd = tablet_fd, .events = POLLIN};
        int rc = poll(&pfd, 1, 50);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll on tablet-mode switch failed: %s\n",
                    strerror(errno));
            libevdev_free(tablet);
            close(tablet_fd);
            tablet = NULL;
            tablet_fd = -1;
            candidate_known = false;
            continue;
        }
        if (rc == 0)
            continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "tablet-mode switch disconnected; reconnecting\n");
            libevdev_free(tablet);
            close(tablet_fd);
            tablet = NULL;
            tablet_fd = -1;
            candidate_known = false;
            continue;
        }
        if (!(pfd.revents & POLLIN))
            continue;

        struct input_event event;
        rc = libevdev_next_event(tablet, LIBEVDEV_READ_FLAG_NORMAL, &event);
        if (rc == -EAGAIN)
            continue;
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            update_candidate(&event, &candidate_known, &candidate_state,
                             &candidate_since, &retry_at);
            do {
                rc = libevdev_next_event(tablet, LIBEVDEV_READ_FLAG_SYNC,
                                         &event);
                if (rc == LIBEVDEV_READ_STATUS_SYNC)
                    update_candidate(&event, &candidate_known,
                                     &candidate_state, &candidate_since,
                                     &retry_at);
            } while (rc == LIBEVDEV_READ_STATUS_SYNC);
            if (rc == -EAGAIN)
                continue;
        }
        if (rc < 0) {
            fprintf(stderr, "reading tablet-mode switch failed: %s; reconnecting\n",
                    strerror(-rc));
            libevdev_free(tablet);
            close(tablet_fd);
            tablet = NULL;
            tablet_fd = -1;
            candidate_known = false;
            continue;
        }
        if (rc != LIBEVDEV_READ_STATUS_SUCCESS || event.type != EV_SW ||
            event.code != SW_TABLET_MODE)
            continue;

        update_candidate(&event, &candidate_known, &candidate_state,
                         &candidate_since, &retry_at);
    }

    if (tablet)
        libevdev_free(tablet);
    if (tablet_fd >= 0)
        close(tablet_fd);
    /* Leave the built-in keyboard usable if the daemon is stopped manually. */
    set_inhibited(&keyboards, false);
    free_keyboard_list(&keyboards);
    return 0;
}
