/*
 * Copyright (C) 2010-2011 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/vt.h>
#endif

#include "vt.h"
#include "configuration.h"

static GList *used_vts = NULL;

GList * vt_get_logined(void);

static gint
open_tty (void)
{
    int fd;

    fd = g_open ("/dev/tty0", O_RDONLY | O_NOCTTY, 0);
    if (fd < 0)
        g_warning ("Error opening /dev/tty0: %s", strerror (errno));
    return fd;
}

gboolean
vt_can_multi_seat (void)
{
    /* Quick check to see if we can multi seat.  This is intentionally the
       same check logind does, just without actually reading from the files.
       Existence will prove whether we have CONFIG_VT built into the kernel. */
    return access ("/dev/tty0", F_OK) == 0 &&
           access ("/sys/class/tty/tty0/active", F_OK) == 0;
}

gint
vt_get_active (void)
{
#ifdef __linux__
    gint tty_fd;
    gint active = -1;

    /* Pretend always active */
    if (getuid () != 0)
        return 1;

    tty_fd = open_tty ();
    if (tty_fd >= 0)
    {
        struct vt_stat vt_state = { 0 };
        if (ioctl (tty_fd, VT_GETSTATE, &vt_state) < 0)
            g_warning ("Error using VT_GETSTATE on /dev/tty0: %s", strerror (errno));
        else
            active = vt_state.v_active;
        close (tty_fd);
    }

    return active;
#else
    return -1;
#endif
}

void
vt_set_active (gint number)
{
#ifdef __linux__
    gint tty_fd;

    g_debug ("Activating VT %d", number);

    /* Pretend always active */
    if (getuid () != 0)
        return;

    tty_fd = open_tty ();
    if (tty_fd >= 0)
    {
        int n = number;

        if (ioctl (tty_fd, VT_ACTIVATE, n) < 0)
        {
            g_warning ("Error using VT_ACTIVATE %d on /dev/tty0: %s", n, strerror (errno));
            close (tty_fd);
            return;
        }

        /* Wait for the VT to become active to avoid a suspected
         * race condition somewhere between LightDM, X, ConsoleKit and the kernel.
         * See https://bugs.launchpad.net/bugs/851612 */
        /* This call sometimes get interrupted (not sure what signal is causing it), so retry if that is the case */
        while (TRUE)
        {
            if (ioctl (tty_fd, VT_WAITACTIVE, n) < 0)
            {
                if (errno == EINTR)
                    continue;
                g_warning ("Error using VT_WAITACTIVE %d on /dev/tty0: %s", n, strerror (errno));
            }
            break;
        }

        close (tty_fd);
    }
#endif
}

gboolean
vt_is_used (gint number)
{
    GList *link;

    for (link = used_vts; link; link = link->next)
    {
        int n = GPOINTER_TO_INT (link->data);
        if (n == number)
            return TRUE;
    }

    gboolean found = FALSE;

    GList *logined_list = vt_get_logined();
    for (GList *l = logined_list; l != NULL; l = l->next)
    {
        if (GPOINTER_TO_INT(l->data) == number)
        {
            found = TRUE;
            break;
        }
    }

    g_list_free(logined_list);

    return found;
}

GList *
vt_get_logined(void)
{
    GList *l = NULL;
    GDir *dir = g_dir_open("/run/systemd/sessions", 0, NULL);
    if (!dir)
        return l;

    const gchar *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL)
    {
        gchar *file = g_strconcat("/run/systemd/sessions/", name, NULL);
        g_debug("vt_get_logined %s", file);
        GError *error = NULL;
        GMappedFile *f = g_mapped_file_new(file, FALSE, &error);
        g_free(file);

        if (error) {
            g_debug("map failed!, %s", error->message);
            continue;
        }

        const gchar *content = g_mapped_file_get_contents(f);
        if (content)
        {
            const gchar * vtnr = strstr(content, "VTNR=");
            if (!vtnr)
                continue;

            vtnr += 5;
            const gchar *start_pos = vtnr;

            int n = 0;
            while (*vtnr++ != '\n')
                ++n;

            gchar *vt_number = g_strndup(start_pos, n);
            int number = atoi(vt_number);
            if (number)
                l = g_list_append(l, GINT_TO_POINTER(number));

            g_free(vt_number);
        }

        g_mapped_file_unref(f);
    }

    g_dir_close(dir);
    return l;
}

gint
vt_get_min (void)
{
    gint number;

    number = config_get_integer (config_get_instance (), "LightDM", "minimum-vt");
    if (number < 1)
        number = 1;

    return number;
}

gint
vt_get_unused (void)
{
    gint number;

    if (getuid () != 0)
        return -1;

    number = vt_get_min ();
    while (vt_is_used (number))
        number++;

    return number;
}

void
vt_ref (gint number)
{
    g_debug ("Using VT %d", number);
    used_vts = g_list_append (used_vts, GINT_TO_POINTER (number));
}

void
vt_unref (gint number)
{
    g_debug ("Releasing VT %d", number);
    used_vts = g_list_remove (used_vts, GINT_TO_POINTER (number));
}
