#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <libinput.h>
#include <math.h>
#include "sway/config.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/server.h"
#include "stringop.h"
#include "list.h"
#include "log.h"

static const char *default_seat = "seat0";

// TODO make me not global
struct sway_input_manager *input_manager;

struct input_config *current_input_config = NULL;
struct seat_config *current_seat_config = NULL;

static struct sway_seat *input_manager_get_seat(
		struct sway_input_manager *input, const char *seat_name) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &input->seats, link) {
		if (strcmp(seat->wlr_seat->name, seat_name) == 0) {
			return seat;
		}
	}

	return sway_seat_create(input, seat_name);
}

static char *get_device_identifier(struct wlr_input_device *device) {
	int vendor = device->vendor;
	int product = device->product;
	char *name = strdup(device->name);
	name = strip_whitespace(name);

	char *p = name;
	for (; *p; ++p) {
		if (*p == ' ') {
			*p = '_';
		}
	}

	const char *fmt = "%d:%d:%s";
	int len = snprintf(NULL, 0, fmt, vendor, product, name) + 1;
	char *identifier = malloc(len);
	if (!identifier) {
		sway_log(L_ERROR, "Unable to allocate unique input device name");
		return NULL;
	}

	snprintf(identifier, len, fmt, vendor, product, name);
	free(name);
	return identifier;
}

static struct sway_input_device *input_sway_device_from_wlr(
		struct sway_input_manager *input, struct wlr_input_device *device) {
	struct sway_input_device *input_device = NULL;
	wl_list_for_each(input_device, &input->devices, link) {
		if (input_device->wlr_device == device) {
			return input_device;
		}
	}
	return NULL;
}

static bool input_has_seat_configuration(struct sway_input_manager *input) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &input->seats, link) {
		if (seat->config) {
			return true;
		}
	}

	return false;
}

static void input_add_notify(struct wl_listener *listener, void *data) {
	struct sway_input_manager *input =
		wl_container_of(listener, input, input_add);
	struct wlr_input_device *device = data;

	struct sway_input_device *input_device =
		calloc(1, sizeof(struct sway_input_device));
	if (!sway_assert(input_device, "could not allocate input device")) {
		return;
	}

	input_device->wlr_device = device;
	input_device->identifier = get_device_identifier(device);
	wl_list_insert(&input->devices, &input_device->link);

	sway_log(L_DEBUG, "adding device: '%s'",
		input_device->identifier);

	// find config
	for (int i = 0; i < config->input_configs->length; ++i) {
		struct input_config *input_config = config->input_configs->items[i];
		if (strcmp(input_config->identifier, input_device->identifier) == 0) {
			input_device->config = input_config;
			break;
		}
	}

	struct sway_seat *seat = NULL;
	if (!input_has_seat_configuration(input)) {
		sway_log(L_DEBUG, "no seat configuration, using default seat");
		seat = input_manager_get_seat(input, default_seat);
		sway_seat_add_device(seat, input_device);
		return;
	}

	bool added = false;
	wl_list_for_each(seat, &input->seats, link) {
		bool has_attachment = seat->config &&
			(seat_config_get_attachment(seat->config, input_device->identifier) ||
			 seat_config_get_attachment(seat->config, "*"));

		if (has_attachment) {
			sway_seat_add_device(seat, input_device);
			added = true;
		}
	}

	if (!added) {
		wl_list_for_each(seat, &input->seats, link) {
			if (seat->config && seat->config->fallback == 1) {
				sway_seat_add_device(seat, input_device);
				added = true;
			}
		}
	}

	if (!added) {
		sway_log(L_DEBUG,
			"device '%s' is not configured on any seats",
			input_device->identifier);
	}
}

static void input_remove_notify(struct wl_listener *listener, void *data) {
	struct sway_input_manager *input =
		wl_container_of(listener, input, input_remove);
	struct wlr_input_device *device = data;

	struct sway_input_device *input_device =
		input_sway_device_from_wlr(input, device);

	if (!sway_assert(input_device, "could not find sway device")) {
		return;
	}

	sway_log(L_DEBUG, "removing device: '%s'",
		input_device->identifier);

	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &input->seats, link) {
		sway_seat_remove_device(seat, input_device);
	}

	wl_list_remove(&input_device->link);
	free(input_device->identifier);
	free(input_device);
}

struct sway_input_manager *sway_input_manager_create(
		struct sway_server *server) {
	struct sway_input_manager *input =
		calloc(1, sizeof(struct sway_input_manager));
	if (!input) {
		return NULL;
	}
	input->server = server;

	wl_list_init(&input->devices);
	wl_list_init(&input->seats);

	// create the default seat
	input_manager_get_seat(input, default_seat);

	input->input_add.notify = input_add_notify;
	wl_signal_add(&server->backend->events.input_add, &input->input_add);

	input->input_remove.notify = input_remove_notify;
	wl_signal_add(&server->backend->events.input_remove, &input->input_remove);

	return input;
}

bool sway_input_manager_has_focus(struct sway_input_manager *input,
		swayc_t *container) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &input->seats, link) {
		if (seat->focus == container) {
			return true;
		}
	}

	return false;
}

void sway_input_manager_set_focus(struct sway_input_manager *input,
		swayc_t *container) {
	struct sway_seat *seat ;
	wl_list_for_each(seat, &input->seats, link) {
		sway_seat_set_focus(seat, container);
	}
}

void sway_input_manager_apply_input_config(struct sway_input_manager *input,
		struct input_config *input_config) {
	struct sway_input_device *input_device = NULL;
	wl_list_for_each(input_device, &input->devices, link) {
		if (strcmp(input_device->identifier, input_config->identifier) == 0) {
			input_device->config = input_config;

			struct sway_seat *seat = NULL;
			wl_list_for_each(seat, &input->seats, link) {
				sway_seat_configure_device(seat, input_device);
			}
		}
	}
}

void sway_input_manager_apply_seat_config(struct sway_input_manager *input,
		struct seat_config *seat_config) {
	sway_log(L_DEBUG, "applying new seat config for seat %s",
		seat_config->name);
	struct sway_seat *seat = input_manager_get_seat(input, seat_config->name);
	if (!seat) {
		return;
	}

	sway_seat_set_config(seat, seat_config);

	// for every device, try to add it to a seat and if no seat has it
	// attached, add it to the fallback seats.
	struct sway_input_device *input_device = NULL;
	wl_list_for_each(input_device, &input->devices, link) {
		list_t *seat_list = create_list();
		struct sway_seat *seat = NULL;
		wl_list_for_each(seat, &input->seats, link) {
			if (!seat->config) {
				continue;
			}
			if (seat_config_get_attachment(seat->config, "*") ||
					seat_config_get_attachment(seat->config,
						input_device->identifier)) {
				list_add(seat_list, seat);
			}
		}

		if (seat_list->length) {
			wl_list_for_each(seat, &input->seats, link) {
				bool attached = false;
				for (int i = 0; i < seat_list->length; ++i) {
					if (seat == seat_list->items[i]) {
						attached = true;
						break;
					}
				}
				if (attached) {
					sway_seat_add_device(seat, input_device);
				} else {
					sway_seat_remove_device(seat, input_device);
				}
			}
		} else {
			wl_list_for_each(seat, &input->seats, link) {
				if (seat->config && seat->config->fallback == 1) {
					sway_seat_add_device(seat, input_device);
				} else {
					sway_seat_remove_device(seat, input_device);
				}
			}
		}
		list_free(seat_list);
	}
}

void sway_input_manager_configure_xcursor(struct sway_input_manager *input) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &input->seats, link) {
		sway_seat_configure_xcursor(seat);
	}
}