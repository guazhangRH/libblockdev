/*
 * Copyright (C) 2015  Red Hat, Inc.
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Vratislav Podzimek <vpodzime@redhat.com>
 */

#include <libkmod.h>
#include <string.h>
#include <syslog.h>
#include <glob.h>
#include <unistd.h>
#include <utils.h>

#include "kbd.h"

/**
 * SECTION: kbd
 * @short_description: plugin for operations with kernel block devices
 * @title: KernelBlockDevices
 * @include: kbd.h
 *
 * A plugin for operations with kernel block devices.
 */

/**
 * bd_kbd_error_quark: (skip)
 */
GQuark bd_kbd_error_quark (void)
{
    return g_quark_from_static_string ("g-bd-kbd-error-quark");
}

static gboolean load_kernel_module (gchar *module_name, gchar *options, GError **error) {
    gint ret = 0;
    struct kmod_ctx *ctx = NULL;
    struct kmod_module *mod = NULL;
    gchar *null_config = NULL;

    ctx = kmod_new (NULL, (const gchar * const*) &null_config);
    if (!ctx) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_KMOD_INIT_FAIL,
                     "Failed to initialize kmod context");
        return FALSE;
    }
    /* prevent libkmod from spamming our STDERR */
    kmod_set_log_priority(ctx, LOG_CRIT);

    ret = kmod_module_new_from_name (ctx, module_name, &mod);
    if (ret < 0) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_FAIL,
                     "Failed to get the module: %s", strerror (-ret));
        kmod_unref (ctx);
        return FALSE;
    }

    if (!kmod_module_get_path (mod)) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_NOEXIST,
                     "Module '%s' doesn't exist", module_name);
        kmod_module_unref (mod);
        kmod_unref (ctx);
        return FALSE;
    }

    /* module, flags, options */
    ret = kmod_module_insert_module (mod, 0, options);
    if (ret < 0) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_FAIL,
                     "Failed to load the module '%s' with options '%s': %s",
                     module_name, options, strerror (-ret));
        kmod_module_unref (mod);
        kmod_unref (ctx);
        return FALSE;
    }

    kmod_module_unref (mod);
    kmod_unref (ctx);
    return TRUE;
}

static gboolean unload_kernel_module (gchar *module_name, GError **error) {
    gint ret = 0;
    struct kmod_ctx *ctx = NULL;
    struct kmod_module *mod = NULL;
    struct kmod_list *list = NULL;
    struct kmod_list *cur = NULL;
    gchar *null_config = NULL;
    gboolean found = FALSE;

    ctx = kmod_new (NULL, (const gchar * const*) &null_config);
    if (!ctx) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_KMOD_INIT_FAIL,
                     "Failed to initialize kmod context");
        return FALSE;
    }
    /* prevent libkmod from spamming our STDERR */
    kmod_set_log_priority(ctx, LOG_CRIT);

    ret = kmod_module_new_from_loaded (ctx, &list);
    if (ret < 0) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_FAIL,
                     "Failed to get the module: %s", strerror (-ret));
        kmod_unref (ctx);
        return FALSE;
    }

    for (cur=list; !found && cur != NULL; cur = kmod_list_next(list, cur)) {
        mod = kmod_module_get_module (cur);
        if (g_strcmp0 (kmod_module_get_name (mod), module_name) == 0)
            found = TRUE;
        else
            kmod_module_unref (mod);
    }

    if (!found) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_NOEXIST,
                     "Module '%s' is not loaded", module_name);
        kmod_unref (ctx);
        return FALSE;
    }

    /* module, flags */
    ret = kmod_module_remove_module (mod, 0);
    if (ret < 0) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_FAIL,
                     "Failed to unload the module '%s': %s",
                     module_name, strerror (-ret));
        kmod_module_unref (mod);
        kmod_unref (ctx);
        return FALSE;
    }

    kmod_module_unref (mod);
    kmod_unref (ctx);
    return TRUE;
}

static gboolean echo_str_to_file (gchar *str, gchar *file_path, GError **error) {
    GIOChannel *out_file = NULL;
    gsize bytes_written = 0;

    out_file = g_io_channel_new_file (file_path, "w", error);
    if (!out_file || g_io_channel_write_chars (out_file, str, -1, &bytes_written, error) != G_IO_STATUS_NORMAL) {
        g_prefix_error (error, "Failed to write '%s' to file '%s': ", str, file_path);
        return FALSE;
    }
    if (g_io_channel_shutdown (out_file, TRUE, error) != G_IO_STATUS_NORMAL) {
        g_prefix_error (error, "Failed to flush and close the file '%s': ", file_path);
        g_io_channel_unref (out_file);
        return FALSE;
    }
    g_io_channel_unref (out_file);
    return TRUE;
}


/**
 * bd_kbd_zram_create_devices:
 * @num_devices: number of devices to create
 * @sizes: (array zero-terminated=1): requested sizes (in bytes) for created zRAM
 *                                    devices
 * @nstreams: (allow-none) (array zero-terminated=1): numbers of streams for created
 *                                                    zRAM devices
 * @error: (out): place to store error (if any)
 *
 * Returns: whether @num_devices zRAM devices were successfully created or not
 *
 * **Lengths of @size and @nstreams (if given) have to be >= @num_devices!**
 */
gboolean bd_kbd_zram_create_devices (guint64 num_devices, guint64 *sizes, guint64 *nstreams, GError **error) {
    gchar *opts = NULL;
    gboolean success = FALSE;
    guint64 i = 0;
    gchar *num_str = NULL;
    gchar *file_name = NULL;

    opts = g_strdup_printf ("num_devices=%"G_GUINT64_FORMAT, num_devices);
    success = load_kernel_module ("zram", opts, error);

    /* maybe it's loaded? Try to unload it first */
    if (!success && g_error_matches (*error, BD_KBD_ERROR, BD_KBD_ERROR_MODULE_FAIL)) {
        g_clear_error (error);
        success = unload_kernel_module ("zram", error);
        if (!success) {
            g_prefix_error (error, "zram module already loaded: ");
            g_free (opts);
            return FALSE;
        }
        success = load_kernel_module ("zram", opts, error);
        if (!success) {
            g_free (opts);
            return FALSE;
        }
    }
    g_free (opts);

    if (!success)
        /* error is already populated */
        return FALSE;

    /* compression streams have to be specified before the device is activated
       by setting its size */
    if (nstreams)
        for (i=0; i < num_devices; i++) {
            file_name = g_strdup_printf ("/sys/block/zram%"G_GUINT64_FORMAT"/max_comp_streams", i);
            num_str = g_strdup_printf ("%"G_GUINT64_FORMAT, nstreams[i]);
            success = echo_str_to_file (num_str, file_name, error);
            g_free (file_name);
            g_free (num_str);
            if (!success) {
                g_prefix_error (error, "Failed to set number of compression streams for '/dev/zram%"G_GUINT64_FORMAT"': ",
                                i);
                return FALSE;
            }
        }

    /* now activate the devices by setting their sizes */
    for (i=0; i < num_devices; i++) {
        file_name = g_strdup_printf ("/sys/block/zram%"G_GUINT64_FORMAT"/disksize", i);
        num_str = g_strdup_printf ("%"G_GUINT64_FORMAT, sizes[i]);
        success = echo_str_to_file (num_str, file_name, error);
        g_free (file_name);
        g_free (num_str);
        if (!success) {
            g_prefix_error (error, "Failed to set size for '/dev/zram%"G_GUINT64_FORMAT"': ",
                            i);
            return FALSE;
        }
    }

    return TRUE;
}

/**
 * bd_kbd_zram_destroy_devices:
 * @error: (out): place to store error (if any)
 *
 * Returns: whether zRAM devices were successfully destroyed or not
 *
 * The only way how to destroy zRAM device right now is to unload the 'zram'
 * module and thus destroy all of them. That's why this function doesn't allow
 * specification of which devices should be destroyed.
 */
gboolean bd_kbd_zram_destroy_devices (GError **error) {
    return unload_kernel_module ("zram", error);
}

/**
 * bd_kbd_bcache_create:
 * @backing_device: backing (slow) device of the cache
 * @cache_device: cache (fast) device of the cache
 * @bcache_device: (out) (allow-none) (transfer full): place to store the name of the new bcache device (if any)
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the bcache device was successfully created or not
 */
gboolean bd_kbd_bcache_create (gchar *backing_device, gchar *cache_device, gchar **bcache_device, GError **error) {
    gchar *argv[6] = {"make-bcache", "-B", backing_device, "-C", cache_device, NULL};
    gboolean success = FALSE;
    gchar *output = NULL;
    gchar **lines = NULL;
    GRegex *regex = NULL;
    GMatchInfo *match_info = NULL;
    gchar *set_uuid = NULL;
    guint i = 0;
    gboolean found = FALSE;
    glob_t globbuf;
    gchar *pattern = NULL;
    gchar *path = NULL;
    gchar *dev_name = NULL;
    gchar *dev_name_end = NULL;

    /* create cache device metadata and try to get Set UUID (needed later) */
    success = bd_utils_exec_and_capture_output (argv, &output, error);
    if (!success) {
        /* error is already populated */
        g_free (output);
        return FALSE;
    }

    lines = g_strsplit (output, "\n", 0);

    regex = g_regex_new ("Set UUID:\\s+([-a-z0-9]+)", 0, 0, error);
    if (!regex) {
        /* error is already populated */
        g_free (output);
        g_strfreev (lines);
        return FALSE;
    }

    for (i=0; !found && lines[i]; i++) {
        success = g_regex_match (regex, lines[i], 0, &match_info);
        if (success) {
            found = TRUE;
            set_uuid = g_match_info_fetch (match_info, 1);
        }
        g_match_info_free (match_info);
    }
    g_regex_unref (regex);
    g_strfreev (lines);

    if (!found) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_BCACHE_PARSE,
                     "Failed to determine Set UUID from: %s", output);
        g_free (output);
        return FALSE;
    }
    g_free (output);


    /* attach the cache device to the backing device */
    /* get the name of the bcache device based on the @backing_device being its slave */
    dev_name = strrchr (backing_device, '/');
    if (!dev_name)
        /* error is already populated */
        return FALSE;
    /* move right after the last '/' (that's where the device name starts) */
    dev_name++;

    /* make sure the bcache device is registered */
    success = echo_str_to_file (backing_device, "/sys/fs/bcache/register", error);
    if (!success)
        /* error is already populated */
        return FALSE;

    pattern = g_strdup_printf ("/sys/block/*/slaves/%s", dev_name);
    if (glob (pattern, GLOB_NOSORT, NULL, &globbuf) != 0) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_BCACHE_SETUP_FAIL,
                     "Failed to determine bcache device name for '%s'", dev_name);
        g_free (pattern);
        return FALSE;
    }
    g_free (pattern);

    /* get the first and only match */
    path = (*globbuf.gl_pathv);

    /* move three '/'s forward */
    dev_name = path + 1;
    for (i=0; i < 2 && dev_name; i++) {
        dev_name = strchr (dev_name, '/');
        dev_name = dev_name ? dev_name + 1: dev_name;
    }
    if (!dev_name) {
        globfree (&globbuf);
        return FALSE;
    }
    /* get everything till the next '/' */
    dev_name_end = strchr (dev_name, '/');
    dev_name = g_strndup (dev_name, (dev_name_end - dev_name));

    globfree (&globbuf);

    success = bd_kbd_bcache_attach (set_uuid, dev_name, error);
    if (!success) {
        g_prefix_error (error, "Failed to attach the cache to the backing device: ");
        g_free (dev_name);
        return FALSE;
    }

    if (bcache_device)
        *bcache_device = dev_name;
    else
        g_free (dev_name);

    return TRUE;
}

/**
 * bd_kbd_bcache_attach:
 * @c_set_uuid: cache set UUID of the cache to attach
 * @bcache_device: bcache device to attach @c_set_uuid cache to
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the @c_set_uuid cache was successfully attached to @bcache_device or not
 */
gboolean bd_kbd_bcache_attach (gchar *c_set_uuid, gchar *bcache_device, GError **error) {
    gchar *path = NULL;
    gboolean success = FALSE;

    if (g_str_has_prefix (bcache_device, "/dev/"))
        bcache_device += 5;

    path = g_strdup_printf ("/sys/block/%s/bcache/attach", bcache_device);
    success = echo_str_to_file (c_set_uuid, path, error);
    g_free (path);

    /* error is already populated (if any) */
    return success;
}

/**
 * bd_kbd_bcache_detach:
 * @bcache_device: bcache device to detach the cache from
 * @c_set_uuid: (out) (allow-none) (transfer full): cache set UUID of the detached cache
 * @error: (out): place to store error (if any)
 * Returns: whether the bcache device @bcache_device was successfully destroyed or not
 *
 * Note: Flushes the cache first.
 */
gboolean bd_kbd_bcache_detach (gchar *bcache_device, gchar **c_set_uuid, GError **error) {
    gchar *path = NULL;
    gchar *link = NULL;
    gchar *uuid = NULL;
    gboolean success = FALSE;

    if (g_str_has_prefix (bcache_device, "/dev/"))
        bcache_device += 5;

    path = g_strdup_printf ("/sys/block/%s/bcache/cache", bcache_device);
    if (access (path, R_OK) != 0) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_BCACHE_NOT_ATTACHED,
                     "No cache attached to '%s' or '%s' not set up", bcache_device, bcache_device);
        g_free (path);
        return FALSE;
    }

    /* if existing, /sys/block/SOME_BCACHE/bcache/cache is a symlink to /sys/fs/bcache/C_SET_UUID */
    link = g_file_read_link (path, error);
    g_free (path);
    if (!link) {
        g_prefix_error (error, "Failed to determine cache set UUID for '%s'", bcache_device);
        return FALSE;
    }

    /* find the last '/' */
    uuid = strrchr (link, '/');
    if (!uuid) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_BCACHE_UUID,
                     "Failed to determine cache set UUID for '%s'", bcache_device);
        g_free (link);
        return FALSE;
    }
    /* move right after the '/' */
    uuid++;

    path = g_strdup_printf ("/sys/block/%s/bcache/detach", bcache_device);
    success = echo_str_to_file (uuid, path, error);
    if (!success) {
        g_set_error (error, BD_KBD_ERROR, BD_KBD_ERROR_BCACHE_DETACH_FAIL,
                     "Failed to detach '%s' from '%s'", uuid, bcache_device);
        g_free (link);
        g_free (path);
        return FALSE;
    }

    if (c_set_uuid)
        *c_set_uuid = g_strdup (uuid);

    g_free (link);
    g_free (path);
    return TRUE;
}

/**
 * bd_kbd_bcache_destroy:
 * @bcache_device: bcache device to destroy
 * @error: (out): place to store error (if any)
 *
 * Returns: whether the bcache device @bcache_device was successfully destroyed or not
 */
gboolean bd_kbd_bcache_destroy (gchar *bcache_device, GError **error) {
    gchar *path = NULL;
    gchar *c_set_uuid = NULL;
    gboolean success = FALSE;

    if (g_str_has_prefix (bcache_device, "/dev/"))
        bcache_device += 5;

    success = bd_kbd_bcache_detach (bcache_device, &c_set_uuid, error);
    if (!success)
        /* error is already populated */
        return FALSE;

    path = g_strdup_printf ("/sys/fs/bcache/%s/stop", c_set_uuid);
    success = echo_str_to_file ("1", path, error);
    g_free (path);
    if (!success) {
        g_prefix_error (error, "Failed to stop the cache set: ");
        return FALSE;
    }

    path = g_strdup_printf ("/sys/block/%s/bcache/stop", bcache_device);
    success = echo_str_to_file ("1", path, error);
    g_free (path);
    if (!success) {
        g_prefix_error (error, "Failed to stop the bcache: ");
        return FALSE;
    }

    return TRUE;
}
