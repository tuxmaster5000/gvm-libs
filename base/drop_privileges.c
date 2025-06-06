/* SPDX-FileCopyrightText: 2010-2023 Greenbone AG
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/**
 * @file
 * @brief Basic support to drop privileges.
 */

#include "drop_privileges.h"

#include <grp.h> /* for initgroups */
#include <pwd.h> /* for passwd, getpwnam */
#include <sys/types.h>
#include <unistd.h> /* for geteuid, setgid, setuid */

#undef G_LOG_DOMAIN
/**
 * @brief GLib log domain.
 */
#define G_LOG_DOMAIN "libgvm base"

/**
 * @brief Sets an error and return \p errorcode
 *
 * @param error     Error to set.
 * @param errorcode Errorcode (possible values defined in drop_privileges.h),
 *                  will be returned.
 * @param message   Message to attach to the error.
 *
 * @return \p errorcode
 */
static gint
drop_privileges_error (GError **error, gint errorcode, const gchar *message)
{
  g_set_error (error, GVM_DROP_PRIVILEGES, errorcode, "%s", message);
  return errorcode;
}

/**
 * @brief Drop privileges.
 *
 * We try to drop our (root) privileges and setuid to \p username to
 * minimize the risk of privilege escalation.
 * The current implementation is linux-specific and may not work on other
 * platforms.
 *
 * @param[in]  username The user to become. Its safe to pass "NULL", in which
 *                      case it will default to "nobody".
 * @param[out] error    Return location for errors or NULL if not interested
 *                      in errors.
 *
 * @return GVM_DROP_PRIVILEGES_OK in case of success. Sets \p error
 *         otherwise and returns the error code.
 */
int
drop_privileges (gchar *username, GError **error)
{
  g_return_val_if_fail (*error == NULL, GVM_DROP_PRIVILEGES_ERROR_ALREADY_SET);

  if (username == NULL)
    username = "nobody";

  if (geteuid () == 0)
    {
      struct passwd *user_pw;

      user_pw = getpwnam (username);
      if (user_pw)
        {
          if (initgroups (username, user_pw->pw_gid) != 0)
            return drop_privileges_error (
              error, GVM_DROP_PRIVILEGES_FAIL_SUPPLEMENTARY,
              "Failed to drop supplementary groups privileges!\n");
          if (setgid (user_pw->pw_gid) != 0)
            return drop_privileges_error (error,
                                          GVM_DROP_PRIVILEGES_FAIL_DROP_GID,
                                          "Failed to drop group privileges!\n");
          if (setuid (user_pw->pw_uid) != 0)
            return drop_privileges_error (error,
                                          GVM_DROP_PRIVILEGES_FAIL_DROP_UID,
                                          "Failed to drop user privileges!\n");
        }
      else
        {
          g_set_error (error, GVM_DROP_PRIVILEGES,
                       GVM_DROP_PRIVILEGES_FAIL_UNKNOWN_USER,
                       "Failed to get gid and uid for user %s.", username);
          return GVM_DROP_PRIVILEGES_FAIL_UNKNOWN_USER;
        }
      return GVM_DROP_PRIVILEGES_OK;
    }
  else
    {
      return drop_privileges_error (error, GVM_DROP_PRIVILEGES_FAIL_NOT_ROOT,
                                    "Only root can drop its privileges.");
    }
}
