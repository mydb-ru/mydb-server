/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "components/audit_log_filter/sys_vars.h"
#include "components/audit_log_filter/audit_error_log.h"
#include "components/audit_log_filter/audit_log_filter.h"
#include "components/audit_log_filter/audit_log_reader.h"

#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/sql_plugin_var.h"

#include <mysql/components/services/component_status_var_service.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/dynamic_privilege.h>
#include <mysql/components/services/mysql_system_variable.h>
#include <mysql/components/services/mysql_thd_store_service.h>
#include <mysql/components/services/security_context.h>
#include <mysql/components/services/system_variable_source.h>

#include <syslog.h>
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <string>
#include <string_view>

namespace audit_log_filter {
namespace {

constexpr std::string_view kCompName{"audit_log_filter"};
const std::string kMaxSizeVarName{"audit_log_filter.max_size"};
const size_t kMaxDbNameLength = 64;

constexpr std::string_view kReaderContextSlotName{
    "component_audit_reader_context"};
constexpr std::string_view kSessionFilterIdSlotName{
    "component_audit_reader_session_filter_id"};
mysql_thd_store_slot reader_context_thread_slot{nullptr};
mysql_thd_store_slot session_filter_id_slot{nullptr};

bool has_system_variables_privilege(MYSQL_THD thd) {
  my_service<SERVICE_TYPE(mysql_thd_security_context)> security_context_service(
      "mysql_thd_security_context", SysVars::get_comp_registry_srv());
  my_service<SERVICE_TYPE(global_grants_check)> grants_check_service(
      "global_grants_check", SysVars::get_comp_registry_srv());

  bool has_audit_admin_grant = false;
  bool has_system_variables_admin_grant = false;

  if (security_context_service.is_valid() && grants_check_service.is_valid()) {
    Security_context_handle ctx;

    if (!security_context_service->get(thd, &ctx)) {
      has_audit_admin_grant = grants_check_service->has_global_grant(
          ctx, STRING_WITH_LEN("AUDIT_ADMIN"));
      has_system_variables_admin_grant = grants_check_service->has_global_grant(
          ctx, STRING_WITH_LEN("SYSTEM_VARIABLES_ADMIN"));
    }
  }

  return has_audit_admin_grant && has_system_variables_admin_grant;
}

/*
 * Internally used variables
 */
std::atomic<uint64_t> record_id{0};
LogBookmark log_bookmark;
std::string encryption_options_id;
bool log_encryption_enabled{false};
comp_registry_srv_container_t comp_registry_srv;

/*
 * Status variables
 */
std::atomic<uint64_t> events_total{0};
std::atomic<uint64_t> events_lost{0};
std::atomic<uint64_t> events_filtered{0};
std::atomic<uint64_t> events_written{0};
std::atomic<uint64_t> write_waits{0};
std::atomic<uint64_t> event_max_drop_size{0};
std::atomic<uint64_t> current_log_size{0};
std::atomic<uint64_t> total_log_size{0};
std::atomic<uint64_t> direct_writes{0};

int show_events_total(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = events_total.load(std::memory_order_relaxed);
  return 0;
}

int show_events_lost(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = events_lost.load(std::memory_order_relaxed);
  return 0;
}

int show_events_filtered(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = events_filtered.load(std::memory_order_relaxed);
  return 0;
}

int show_events_written(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = events_written.load(std::memory_order_relaxed);
  return 0;
}

int show_write_waits(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = write_waits.load(std::memory_order_relaxed);
  return 0;
}

int show_event_max_drop_size(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = event_max_drop_size.load(std::memory_order_relaxed);
  return 0;
}

int show_current_log_size(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = current_log_size.load(std::memory_order_relaxed);
  return 0;
}

int show_total_log_size(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = total_log_size.load(std::memory_order_relaxed);
  return 0;
}

int show_direct_writes(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  auto *value = reinterpret_cast<uint64_t *>(buff);
  *value = direct_writes.load(std::memory_order_relaxed);
  return 0;
}

SHOW_VAR status_vars[] = {
    {"Audit_log_filter_events", reinterpret_cast<char *>(&show_events_total),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_events_lost",
     reinterpret_cast<char *>(&show_events_lost), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_events_filtered",
     reinterpret_cast<char *>(&show_events_filtered), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_events_written",
     reinterpret_cast<char *>(&show_events_written), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_write_waits",
     reinterpret_cast<char *>(&show_write_waits), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_event_max_drop_size",
     reinterpret_cast<char *>(&show_event_max_drop_size), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_current_size",
     reinterpret_cast<char *>(&show_current_log_size), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_total_size",
     reinterpret_cast<char *>(&show_total_log_size), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Audit_log_filter_direct_writes",
     reinterpret_cast<char *>(&show_direct_writes), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

/*
 * System variables
 */
char *log_file_full_path;
std::string default_log_file_name{"audit_filter.log"};
char *config_database_name;
std::string default_config_database_name{"mysql"};
ulong log_handler_type = static_cast<ulong>(AuditLogHandlerType::File);
ulong log_format_type = static_cast<ulong>(AuditLogFormatType::New);
ulong log_strategy_type =
    static_cast<ulong>(AuditLogStrategyType::Asynchronous);
ulonglong log_write_buffer_size = 1048576UL;
ulonglong log_rotate_on_size = 0;
constexpr ulonglong default_log_rotate_on_size = 1024 * 1024 * 1024;
ulonglong log_max_size = 0;
constexpr ulonglong default_log_max_size = 1024 * 1024 * 1024;
ulonglong log_prune_seconds = 0;
bool log_disabled = false;
char *log_syslog_tag = nullptr;
std::string default_log_syslog_tag{"audit-filter"};
ulong log_syslog_facility = 0;
ulong log_syslog_priority = 0;
ulong log_compression_type = static_cast<ulong>(AuditLogCompressionType::None);
ulong log_encryption_type = static_cast<ulong>(AuditLogEncryptionType::None);
ulonglong log_password_history_keep_days = 0;
int key_derivation_iter_count_mean = 0;
const int default_key_derivation_iter_count_mean = 600000;
bool json_with_unix_timestamp = false;
ulong read_buffer_size = 0;

const char *audit_log_filter_handler_names[] = {"FILE", "SYSLOG", nullptr};
TYPE_LIB audit_log_filter_handler_typelib = {
    array_elements(audit_log_filter_handler_names) - 1,
    "audit_log_filter_handler_typelib", audit_log_filter_handler_names,
    nullptr};

const char *audit_log_filter_format_names[] = {"NEW", "OLD", "JSON", nullptr};
TYPE_LIB audit_log_filter_format_typelib = {
    array_elements(audit_log_filter_format_names) - 1,
    "audit_log_filter_format_typelib", audit_log_filter_format_names, nullptr};

const char *audit_log_filter_strategy_names[] = {
    "ASYNCHRONOUS", "PERFORMANCE", "SEMISYNCHRONOUS", "SYNCHRONOUS", nullptr};
TYPE_LIB audit_log_filter_strategy_typelib = {
    array_elements(audit_log_filter_strategy_names) - 1,
    "audit_log_filter_strategy_typelib", audit_log_filter_strategy_names,
    nullptr};

void max_size_update_func(MYSQL_THD, SYS_VAR *, void *val_ptr,
                          const void *save) {
  const auto *val = static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(val_ptr) = *val;

  if (*val > 0) {
    log_prune_seconds = 0;
    get_audit_log_filter_instance()->on_audit_log_prune_requested();
  }
}

void prune_seconds_update_func(MYSQL_THD, SYS_VAR *, void *val_ptr,
                               const void *save) {
  const auto *val = static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(val_ptr) = *val;

  if (*val > 0) {
    log_max_size = 0;
    get_audit_log_filter_instance()->on_audit_log_prune_requested();
  }
}

const int audit_log_filter_syslog_facility_codes[] = {
    LOG_USER,     LOG_AUTHPRIV, LOG_CRON,   LOG_DAEMON, LOG_FTP,    LOG_KERN,
    LOG_LPR,      LOG_MAIL,     LOG_NEWS,
#if (defined LOG_SECURITY)
    LOG_SECURITY,
#endif
    LOG_SYSLOG,   LOG_AUTH,     LOG_UUCP,   LOG_LOCAL0, LOG_LOCAL1, LOG_LOCAL2,
    LOG_LOCAL3,   LOG_LOCAL4,   LOG_LOCAL5, LOG_LOCAL6, LOG_LOCAL7, 0};

const char *audit_log_filter_syslog_facility_names[] = {
    "LOG_USER",     "LOG_AUTHPRIV", "LOG_CRON",   "LOG_DAEMON", "LOG_FTP",
    "LOG_KERN",     "LOG_LPR",      "LOG_MAIL",   "LOG_NEWS",
#if (defined LOG_SECURITY)
    "LOG_SECURITY",
#endif
    "LOG_SYSLOG",   "LOG_AUTH",     "LOG_UUCP",   "LOG_LOCAL0", "LOG_LOCAL1",
    "LOG_LOCAL2",   "LOG_LOCAL3",   "LOG_LOCAL4", "LOG_LOCAL5", "LOG_LOCAL6",
    "LOG_LOCAL7",   nullptr};

TYPE_LIB audit_log_filter_syslog_facility_typelib = {
    array_elements(audit_log_filter_syslog_facility_names) - 1,
    "audit_log_filter_syslog_facility_typelib",
    audit_log_filter_syslog_facility_names, nullptr};

const int audit_log_filter_syslog_priority_codes[] = {
    LOG_INFO,   LOG_ALERT, LOG_CRIT,  LOG_ERR, LOG_WARNING,
    LOG_NOTICE, LOG_EMERG, LOG_DEBUG, 0};

const char *audit_log_filter_syslog_priority_names[] = {
    "LOG_INFO",   "LOG_ALERT", "LOG_CRIT",  "LOG_ERR", "LOG_WARNING",
    "LOG_NOTICE", "LOG_EMERG", "LOG_DEBUG", 0};

TYPE_LIB audit_log_filter_syslog_priority_typelib = {
    array_elements(audit_log_filter_syslog_priority_names) - 1,
    "audit_log_filter_syslog_priority_typelib",
    audit_log_filter_syslog_priority_names, nullptr};

int log_disabled_check_func(MYSQL_THD thd, SYS_VAR *var, void *save,
                            st_mysql_value *value) {
  if (!has_system_variables_privilege(thd)) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SYSTEM_VARIABLES_ADMIN and AUDIT_ADMIN");
    return 1;
  }

  return check_func_bool(thd, var, save, value);
}

const char *audit_log_filter_compression_names[] = {"NONE", "GZIP", nullptr};
TYPE_LIB audit_log_filter_compression_typelib = {
    array_elements(audit_log_filter_compression_names) - 1,
    "audit_log_filter_compression_typelib", audit_log_filter_compression_names,
    nullptr};

const char *audit_log_filter_encryption_names[] = {"NONE", "AES", nullptr};
TYPE_LIB audit_log_filter_encryption_typelib = {
    array_elements(audit_log_filter_encryption_names) - 1,
    "audit_log_filter_encryption_typelib", audit_log_filter_encryption_names,
    nullptr};

int password_history_keep_days_check_func(MYSQL_THD thd, SYS_VAR *var,
                                          void *save, st_mysql_value *value) {
  my_service<SERVICE_TYPE(mysql_thd_security_context)> security_context_service(
      "mysql_thd_security_context", SysVars::get_comp_registry_srv());
  my_service<SERVICE_TYPE(global_grants_check)> grants_check_service(
      "global_grants_check", SysVars::get_comp_registry_srv());

  bool has_audit_admin_grant = false;

  if (security_context_service.is_valid() && grants_check_service.is_valid()) {
    Security_context_handle ctx;

    if (!security_context_service->get(thd, &ctx)) {
      has_audit_admin_grant = grants_check_service->has_global_grant(
          ctx, STRING_WITH_LEN("AUDIT_ADMIN"));
    }
  }

  if (!has_audit_admin_grant) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), "AUDIT_ADMIN");
    return 1;
  }

  return check_func_longlong(thd, var, save, value);
}

void password_history_keep_days_update_func(MYSQL_THD, SYS_VAR *, void *val_ptr,
                                            const void *save) {
  const auto *val = static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(val_ptr) = *val;
  get_audit_log_filter_instance()->on_encryption_password_prune_requested();
}

int format_unix_timestamp_check_func(MYSQL_THD thd, SYS_VAR *var, void *save,
                                     st_mysql_value *value) {
  if (!has_system_variables_privilege(thd)) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             "SYSTEM_VARIABLES_ADMIN and AUDIT_ADMIN");
    return 1;
  }

  return check_func_bool(thd, var, save, value);
}

void format_unix_timestamp_update_func(MYSQL_THD, SYS_VAR *, void *val_ptr,
                                       const void *save) {
  const auto new_val = *static_cast<const bool *>(save);

  if (json_with_unix_timestamp != new_val) {
    *static_cast<bool *>(val_ptr) = new_val;

    if (SysVars::get_format_type() == AuditLogFormatType::Json) {
      get_audit_log_filter_instance()->on_audit_log_rotate_requested();
    }
  }
}

using bool_arg_check_type = BOOL_CHECK_ARG(bool);
using str_arg_check_type = STR_CHECK_ARG(str);
using enum_arg_check_type = ENUM_CHECK_ARG(type_lib);
using ulonglong_arg_check_type = INTEGRAL_CHECK_ARG(ulonglong);
using ulong_arg_check_type = INTEGRAL_CHECK_ARG(ulong);
using int_arg_check_type = INTEGRAL_CHECK_ARG(int);

str_arg_check_type check_file{default_log_file_name.data()};
enum_arg_check_type check_handler{static_cast<ulong>(AuditLogHandlerType::File),
                                  &audit_log_filter_handler_typelib};
enum_arg_check_type check_format{static_cast<ulong>(AuditLogFormatType::New),
                                 &audit_log_filter_format_typelib};
enum_arg_check_type check_strategy{
    static_cast<ulong>(AuditLogStrategyType::Asynchronous),
    &audit_log_filter_strategy_typelib};
ulonglong_arg_check_type check_buffer_size{1048576UL, 4096UL, ULLONG_MAX,
                                           4096UL};
ulonglong_arg_check_type check_rotate_on_size{default_log_rotate_on_size, 0UL,
                                              ULLONG_MAX, 4096UL};
ulonglong_arg_check_type check_max_size{default_log_max_size, 0UL, ULLONG_MAX,
                                        4096UL};
ulonglong_arg_check_type check_prune_seconds{0UL, 0UL, ULLONG_MAX, 0UL};
str_arg_check_type check_syslog_tag{default_log_syslog_tag.data()};
enum_arg_check_type check_syslog_facility{
    0, &audit_log_filter_syslog_facility_typelib};
enum_arg_check_type check_syslog_priority{
    0, &audit_log_filter_syslog_priority_typelib};
bool_arg_check_type check_disable{false};
enum_arg_check_type check_compression{
    static_cast<ulong>(AuditLogCompressionType::None),
    &audit_log_filter_compression_typelib};
enum_arg_check_type check_encryption{
    static_cast<ulong>(AuditLogEncryptionType::None),
    &audit_log_filter_encryption_typelib};
ulonglong_arg_check_type check_password_history_keep_days{0UL, 0UL, ULLONG_MAX,
                                                          0UL};
int_arg_check_type check_key_derivation_iterations_count_mean{
    default_key_derivation_iter_count_mean, 1000, 1000000, 0};
bool_arg_check_type check_format_unix_timestamp{false};
str_arg_check_type check_database{default_config_database_name.data()};
ulong_arg_check_type check_read_buffer_size{32768UL, 32768UL, ULONG_MAX, 0UL};

struct SysVarInfo {
  const char *name;
  int flags;
  const char *comment;
  mysql_sys_var_check_func check;
  mysql_sys_var_update_func update;
  void *check_arg;
  void *variable_value;
};
using SysVarListType = std::vector<std::pair<SysVarInfo, bool>>;

SysVarListType sys_vars = {
    /*
     * The audit_log_filter.file variable is used to specify the filename that’s
     * going to store the audit log. It can contain the path relative to the
     * datadir or absolute path.
     */
    {{"file",
      PLUGIN_VAR_STR | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
          PLUGIN_VAR_MEMALLOC,
      "The name of the log file.", nullptr, nullptr,
      static_cast<void *>(&check_file),
      static_cast<void *>(&log_file_full_path)},
     false},
    /*
     * The audit_log_filter.handler variable is used to configure where the
     * audit log will be written. If it is set to FILE, the log will be written
     * into a file specified by audit_log_filter.file variable. If it is set to
     * SYSLOG, the audit log will be written to syslog.
     */
    {{"handler", PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "The audit log handler.", nullptr, nullptr,
      static_cast<void *>(&check_handler),
      static_cast<void *>(&log_handler_type)},
     false},
    /*
     * The audit_log_filter.format variable is used to specify the audit filter
     * log format. The audit log filter plugin supports three log formats:
     * OLD, NEW and JSON. OLD and NEW formats are based on XML, where
     * the former outputs log record properties as XML attributes and the latter
     * as XML tags.
     */
    {{"format", PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "The audit log file format.", nullptr, nullptr,
      static_cast<void *>(&check_format),
      static_cast<void *>(&log_format_type)},
     false},
    /*
     * The audit_log_filter.strategy variable is used to specify the audit log
     * filter strategy, possible values are:
     * ASYNCHRONOUS - (default) log using memory buffer, do not drop messages
     *                if buffer is full
     * PERFORMANCE - log using memory buffer, drop messages if buffer is full
     * SEMISYNCHRONOUS - log directly to file, do not flush and sync every event
     * SYNCHRONOUS - log directly to file, flush and sync every event.
     *
     * This variable has effect only when audit_log_handler is set to FILE.
     */
    {{"strategy", PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "The logging method used by the audit log plugin, if FILE handler is "
      "used.",
      nullptr, nullptr, static_cast<void *>(&check_strategy),
      static_cast<void *>(&log_strategy_type)},
     false},
    /*
     * The audit_log_filter.buffer_size variable can be used to specify the size
     * of memory buffer used for logging, used when audit_log_filter.strategy
     * variable is set to ASYNCHRONOUS or PERFORMANCE values. This variable has
     * effect only when audit_log_filter.handler is set to FILE.
     */
    {{"buffer_size",
      PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_RQCMDARG |
          PLUGIN_VAR_READONLY,
      "The size of the buffer for asynchronous logging, if FILE handler is "
      "used.",
      nullptr, nullptr, static_cast<void *>(&check_buffer_size),
      static_cast<void *>(&log_write_buffer_size)},
     false},
    /*
     * The audit_log_filter.rotate_on_size variable specifies the maximum size
     * of the audit log file in bytes. Upon reaching this size, the audit log
     * will be rotated. For this variable to take effect, set the
     * audit_log_filter.handler variable to FILE.
     */
    {{"rotate_on_size",
      PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_RQCMDARG,
      "Maximum size of the log to start the rotation in bytes, if FILE handler "
      "is used.",
      nullptr, nullptr, static_cast<void *>(&check_rotate_on_size),
      static_cast<void *>(&log_rotate_on_size)},
     false},
    /*
     * The audit_log_filter.max_size enables size-based pruning when set to a
     * value greater than 0. The value is the combined size in bytes above
     * which audit log files become subject to pruning.
     */
    {{"max_size",
      PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_OPCMDARG,
      "The maximum combined size of log files in bytes after which log files "
      "become subject to pruning.",
      nullptr, max_size_update_func, static_cast<void *>(&check_max_size),
      static_cast<void *>(&log_max_size)},
     false},
    /*
     * The audit_log_filter.prune_seconds enables age-based pruning when set to
     * a value greater than 0. The value is the number of seconds after which
     * log files become subject to pruning.
     */
    {{"prune_seconds",
      PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_OPCMDARG,
      "The maximum log file age in seconds after which log file become subject "
      "to pruning.",
      nullptr, prune_seconds_update_func,
      static_cast<void *>(&check_prune_seconds),
      static_cast<void *>(&log_prune_seconds)},
     false},
    /*
     * The audit_log_filter.syslog_tag variable is used to specify the prefix
     * used for syslog messages.
     */
    {{"syslog_tag",
      PLUGIN_VAR_STR | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
          PLUGIN_VAR_MEMALLOC,
      "The string that will be prepended to each log message, if SYSLOG "
      "handler is used.",
      nullptr, nullptr, static_cast<void *>(&check_syslog_tag),
      static_cast<void *>(&log_syslog_tag)},
     false},
    /*
     * The audit_log_filter.syslog_facility variable is used to specify the
     * facility value for syslog.
     */
    {{"syslog_facility",
      PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "The syslog facility to assign to messages, if SYSLOG handler is used.",
      nullptr, nullptr, static_cast<void *>(&check_syslog_facility),
      static_cast<void *>(&log_syslog_facility)},
     false},
    /*
     * The audit_log_filter.syslog_priority variable is used to specify the
     * priority value for syslog. This variable has the same meaning as the
     * appropriate parameter described in the syslog(3) manual.
     */
    {{"syslog_priority",
      PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "Priority to be assigned to all messages written to syslog.", nullptr,
      nullptr, static_cast<void *>(&check_syslog_priority),
      static_cast<void *>(&log_syslog_priority)},
     false},
    /*
     * The audit_log_filter.disable variable permits disabling audit logging
     * for all connecting and connected sessions.
     */
    {{"disable", PLUGIN_VAR_BOOL | PLUGIN_VAR_RQCMDARG,
      "Disable audit logging for all connecting and connected sessions.",
      log_disabled_check_func, nullptr, static_cast<void *>(&check_disable),
      static_cast<void *>(&log_disabled)},
     false},
    /*
     * The audit_log_filter.compression variable specifies type of compression
     * for the audit log file, possible values are:
     * NONE - (default) no compression
     * GZIP - GNU Zip compression.
     */
    {{"compression",
      PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "The type of compression for the audit log file.", nullptr, nullptr,
      static_cast<void *>(&check_compression),
      static_cast<void *>(&log_compression_type)},
     false},
    /*
     * The audit_log_filter.encryption variable specifies type of encryption
     * for the audit log file, possible values are:
     * NONE - (default) no encryption
     * AES - AES-256-CBC cipher encryption.
     */
    {{"encryption", PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "The type of encryption for the audit log file.", nullptr, nullptr,
      static_cast<void *>(&check_encryption),
      static_cast<void *>(&log_encryption_type)},
     false},
    /*
     * The audit_log_filter.password_history_keep_days variable controls
     * automatic removal of expired archived passwords. It indicates the number
     * of days after which archived audit log encryption passwords are removed.
     */
    {{"password_history_keep_days",
      PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_OPCMDARG,
      "Indicates the number of days after which archived audit log encryption "
      "passwords are removed.",
      password_history_keep_days_check_func,
      password_history_keep_days_update_func,
      static_cast<void *>(&check_password_history_keep_days),
      static_cast<void *>(&log_password_history_keep_days)},
     false},
    /*
     * The audit_log_filter.key_derivation_iterations_count_mean variable
     * specifies the mean value of number of iterations used by password based
     * derivation routine while calculating encryption key and iv values.
     * Actual iterations count is calculated as random number which deviates
     * not more than 10% from this value.
     */
    {{"key_derivation_iterations_count_mean",
      PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_OPCMDARG,
      "Mean value of randomly generated iterations count used by password "
      "based derivation routine.",
      nullptr, nullptr,
      static_cast<void *>(&check_key_derivation_iterations_count_mean),
      static_cast<void *>(&key_derivation_iter_count_mean)},
     false},
    /*
     * The audit_log_filter.format_unix_timestamp variable when enabled causes
     * each log file record to include a time field. The field value is an
     * integer that represents the UNIX timestamp value indicating the date
     * and time when the audit event was generated. Applies to JSON formatted
     * logs only.
     */
    {{"format_unix_timestamp", PLUGIN_VAR_BOOL | PLUGIN_VAR_RQCMDARG,
      "Add 'time' field to JSON formatted log records representing the UNIX "
      "timestamp value indicating the date and time when the audit event was "
      "generated.",
      format_unix_timestamp_check_func, format_unix_timestamp_update_func,
      static_cast<void *>(&check_format_unix_timestamp),
      static_cast<void *>(&json_with_unix_timestamp)},
     false},
    /*
     * The audit_log_filter.database variable specifies which database the
     * plugin uses to find its tables. Defaults to 'mysql'.
     */
    {{"database",
      PLUGIN_VAR_STR | PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY |
          PLUGIN_VAR_MEMALLOC,
      "Specifies which database the plugin uses to find its tables.", nullptr,
      nullptr, static_cast<void *>(&check_database),
      static_cast<void *>(&config_database_name)},
     false},
    /*
     * The audit_log_filter.read_buffer_size variable defines buffer size for
     * reading from the audit log file, in bytes. The audit_log_read() function
     * reads no more than this many bytes. Log file reading is supported only
     * for JSON log format.
     */
    {{"read_buffer_size",
      PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_RQCMDARG,
      "The buffer size for reading from the audit log file, in bytes.", nullptr,
      nullptr, static_cast<void *>(&check_read_buffer_size),
      static_cast<void *>(&read_buffer_size)},
     false}};

#ifndef NDEBUG
auto get_initial_debug_time_point() {
  std::tm tm = {};
  std::stringstream{"2022-08-09 10:00:00"} >>
      std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
  return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}
#endif

std::string get_log_dir_name_value(const char *full_path) {
  std::filesystem::path log_path{full_path};

  if (log_path.is_absolute()) {
    return log_path.has_parent_path() ? log_path.parent_path().string()
                                      : mysql_data_home;
  }

  return log_path.has_parent_path()
             ? std::filesystem::path{std::filesystem::path{mysql_data_home} /
                                     log_path.parent_path()}
                   .string()
             : mysql_data_home;
}

std::string get_log_file_name_value(const char *full_path) {
  std::filesystem::path log_path{full_path};
  return log_path.has_filename() ? log_path.filename().string()
                                 : default_log_file_name;
}

}  // namespace

bool SysVars::init() noexcept {
  my_service<SERVICE_TYPE(status_variable_registration)>
      status_var_registration_srv("status_variable_registration",
                                  SysVars::get_comp_registry_srv());
  my_service<SERVICE_TYPE(component_sys_variable_register)>
      sys_var_registration_srv("component_sys_variable_register",
                               SysVars::get_comp_registry_srv());
  my_service<SERVICE_TYPE(mysql_thd_store)> thd_store_service(
      "mysql_thd_store", SysVars::get_comp_registry_srv());

  if (thd_store_service->register_slot(
          kReaderContextSlotName.data(),
          [](void *ctx) -> int {
            if (ctx != nullptr) {
              delete reinterpret_cast<AuditLogReaderContext *>(ctx);
            }
            return 0;
          },
          &reader_context_thread_slot) == 1) {
    return false;
  }

  if (thd_store_service->register_slot(
          kSessionFilterIdSlotName.data(),
          [](void *id) -> int {
            if (id != nullptr) {
              delete reinterpret_cast<ulong *>(id);
            }
            return 0;
          },
          &session_filter_id_slot) == 1) {
    return false;
  }

  if (status_var_registration_srv->register_variable(status_vars) == 1) {
    LogComponentErr(ERROR_LEVEL, ER_AUDIT_STATUS_VAR_REGISTER_FAILURE);
    SysVars::deinit();
    return false;
  }

  for (auto &var : sys_vars) {
    if (sys_var_registration_srv->register_variable(
            kCompName.data(), var.first.name, var.first.flags,
            var.first.comment, var.first.check, var.first.update,
            var.first.check_arg, var.first.variable_value) == 1) {
      LogComponentErr(ERROR_LEVEL, ER_AUDIT_SYS_VAR_REGISTER_FAILURE,
                      kCompName.data(), var.first.name);
      SysVars::deinit();
      return false;
    }

    var.second = true;
  }

  return true;
}

void SysVars::deinit() noexcept {
  my_service<SERVICE_TYPE(status_variable_registration)>
      status_var_registration_srv("status_variable_registration",
                                  SysVars::get_comp_registry_srv());
  my_service<SERVICE_TYPE(component_sys_variable_unregister)>
      sys_var_registration_srv("component_sys_variable_unregister",
                               SysVars::get_comp_registry_srv());
  my_service<SERVICE_TYPE(mysql_thd_store)> thd_store_service(
      "mysql_thd_store", SysVars::get_comp_registry_srv());

  // Current THD instance may still hold allocated resources which will not
  // be released properly after unregistering slots below. Cleaning them up
  // manually before slots removal.
  delete reinterpret_cast<AuditLogReaderContext *>(
      thd_store_service->get(nullptr, reader_context_thread_slot));
  delete reinterpret_cast<ulong *>(
      thd_store_service->get(nullptr, session_filter_id_slot));
  thd_store_service->set(nullptr, reader_context_thread_slot, nullptr);
  thd_store_service->set(nullptr, session_filter_id_slot, nullptr);

  thd_store_service->unregister_slot(reader_context_thread_slot);
  thd_store_service->unregister_slot(session_filter_id_slot);

  if (status_var_registration_srv->unregister_variable(status_vars) == 1) {
    LogComponentErr(ERROR_LEVEL, ER_AUDIT_STATUS_VAR_UNREGISTER_FAILURE);
  }

  for (auto &var : sys_vars) {
    if (var.second && sys_var_registration_srv->unregister_variable(
                          kCompName.data(), var.first.name) == 1) {
      LogComponentErr(ERROR_LEVEL, ER_AUDIT_SYS_VAR_UNREGISTER_FAILURE,
                      kCompName.data(), var.first.name);
    }
    var.second = false;
  }
}

bool SysVars::validate() noexcept {
  const auto *db_name = get_config_database_name();

  if (db_name == nullptr || strlen(db_name) == 0 ||
      strlen(db_name) > kMaxDbNameLength) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Bad audit_log_filter.database value");
    return false;
  }

  my_service<SERVICE_TYPE(system_variable_source)> sysvar_source_service(
      "system_variable_source", SysVars::get_comp_registry_srv());

  enum_variable_source log_max_size_source;

  if (sysvar_source_service->get(kMaxSizeVarName.c_str(),
                                 kMaxSizeVarName.length(),
                                 &log_max_size_source)) {
    LogComponentErr(ERROR_LEVEL, ER_AUDIT_SYS_VAR_SOURCE_CHECK_FAILURE,
                    kMaxSizeVarName.c_str());
    return false;
  }

  if (log_max_size_source == COMPILED && SysVars::get_log_prune_seconds() > 0) {
    // Clean default settings for max_size in case non-zero value for
    // prune_seconds is provided
    log_max_size = 0;
  }

  if (SysVars::get_log_max_size() > 0 && SysVars::get_log_prune_seconds() > 0) {
    LogComponentErr(
        WARNING_LEVEL, ER_LOG_PRINTF_MSG,
        "Both audit_log_filter.max_size and audit_log_filter.prune_seconds are "
        "set to non-zero, audit_log_filter.max_size takes precedence and "
        "audit_log_filter.prune_seconds is ignored");
  }

  return true;
}

const std::string &SysVars::get_file_dir() noexcept {
  static std::string log_dir_name{get_log_dir_name_value(log_file_full_path)};
  return log_dir_name;
}

const std::string &SysVars::get_file_name() noexcept {
  static std::string log_file_name{get_log_file_name_value(log_file_full_path)};
  return log_file_name;
}

const char *SysVars::get_config_database_name() noexcept {
  return config_database_name;
}

AuditLogHandlerType SysVars::get_handler_type() noexcept {
  return static_cast<AuditLogHandlerType>(log_handler_type);
}

AuditLogFormatType SysVars::get_format_type() noexcept {
  return static_cast<AuditLogFormatType>(log_format_type);
}

AuditLogStrategyType SysVars::get_file_strategy_type() noexcept {
  return static_cast<AuditLogStrategyType>(log_strategy_type);
}

ulonglong SysVars::get_buffer_size() noexcept { return log_write_buffer_size; }

ulonglong SysVars::get_rotate_on_size() noexcept { return log_rotate_on_size; }

ulonglong SysVars::get_log_max_size() noexcept { return log_max_size; }

ulonglong SysVars::get_log_prune_seconds() noexcept {
  return log_prune_seconds;
}

const char *SysVars::get_syslog_tag() noexcept { return log_syslog_tag; }

int SysVars::get_syslog_facility() noexcept {
  return audit_log_filter_syslog_facility_codes[log_syslog_facility];
}

int SysVars::get_syslog_priority() noexcept {
  return audit_log_filter_syslog_priority_codes[log_syslog_priority];
}

AuditLogCompressionType SysVars::get_compression_type() noexcept {
  return static_cast<AuditLogCompressionType>(log_compression_type);
}

AuditLogEncryptionType SysVars::get_encryption_type() noexcept {
  return static_cast<AuditLogEncryptionType>(log_encryption_type);
}

void SysVars::set_log_encryption_enabled(bool is_enabled) noexcept {
  log_encryption_enabled = is_enabled;
}

bool SysVars::get_log_encryption_enabled() noexcept {
  return log_encryption_enabled;
}

ulonglong SysVars::get_password_history_keep_days() noexcept {
  return log_password_history_keep_days;
}

int SysVars::get_key_derivation_iter_count_mean() noexcept {
  return key_derivation_iter_count_mean;
}

bool SysVars::get_format_unix_timestamp() noexcept {
  return json_with_unix_timestamp;
}

void SysVars::set_session_filter_id(MYSQL_THD thd, ulong id) noexcept {
  my_service<SERVICE_TYPE(mysql_thd_store)> thd_store_service(
      "mysql_thd_store", SysVars::get_comp_registry_srv());

  auto *id_ptr = reinterpret_cast<ulong *>(
      thd_store_service->get(thd, session_filter_id_slot));

  if (id_ptr == nullptr) {
    auto *local_ptr = new (std::nothrow) ulong{id};

    if (local_ptr != nullptr) {
      if (thd_store_service->set(thd, session_filter_id_slot,
                                 reinterpret_cast<void *>(local_ptr)) == 1) {
        LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                        "Failed to set session_filter_id");
        delete local_ptr;
      }
    } else {
      LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                      "Failed to allocate session_filter_id");
    }
  } else {
    *id_ptr = id;
  }
}

ulong SysVars::get_session_filter_id(THD *thd) noexcept {
  my_service<SERVICE_TYPE(mysql_thd_store)> thd_store_service(
      "mysql_thd_store", SysVars::get_comp_registry_srv());

  auto *id = reinterpret_cast<ulong *>(
      thd_store_service->get(thd, session_filter_id_slot));
  return id == nullptr ? 0 : *id;
}

bool SysVars::get_log_disabled() noexcept { return log_disabled; }

ulong SysVars::get_read_buffer_size(MYSQL_THD thd [[maybe_unused]]) noexcept {
  return read_buffer_size;
}

void SysVars::update_log_bookmark(uint64_t id,
                                  const std::string &timestamp) noexcept {
  log_bookmark.id = id;
  log_bookmark.timestamp = timestamp;
}

LogBookmark SysVars::get_log_bookmark() noexcept { return log_bookmark; }

AuditLogReaderContext *SysVars::get_log_reader_context(MYSQL_THD thd) noexcept {
  my_service<SERVICE_TYPE(mysql_thd_store)> thd_store_service(
      "mysql_thd_store", SysVars::get_comp_registry_srv());

  return reinterpret_cast<AuditLogReaderContext *>(
      thd_store_service->get(thd, reader_context_thread_slot));
}

void SysVars::set_log_reader_context(MYSQL_THD thd,
                                     AuditLogReaderContext *context) noexcept {
  my_service<SERVICE_TYPE(mysql_thd_store)> thd_store_service(
      "mysql_thd_store", SysVars::get_comp_registry_srv());

  thd_store_service->set(thd, reader_context_thread_slot,
                         reinterpret_cast<void *>(context));
}

void SysVars::inc_events_total() noexcept {
  events_total.fetch_add(1, std::memory_order_relaxed);
}

void SysVars::inc_events_lost() noexcept {
  events_lost.fetch_add(1, std::memory_order_relaxed);
}

void SysVars::inc_events_filtered() noexcept {
  events_filtered.fetch_add(1, std::memory_order_relaxed);
}

void SysVars::inc_events_written() noexcept {
  events_written.fetch_add(1, std::memory_order_relaxed);
}

void SysVars::inc_write_waits() noexcept {
  write_waits.fetch_add(1, std::memory_order_relaxed);
}

void SysVars::update_event_max_drop_size(uint64_t size) noexcept {
  uint64_t prev_max_size = event_max_drop_size.load();
  while (prev_max_size < size &&
         !event_max_drop_size.compare_exchange_weak(prev_max_size, size)) {
  }
}

void SysVars::set_current_log_size(uint64_t size) noexcept {
  uint64_t current_size = current_log_size.load();
  while (!current_log_size.compare_exchange_weak(current_size, size)) {
  }
}

void SysVars::update_current_log_size(uint64_t size) noexcept {
  current_log_size.fetch_add(size, std::memory_order_relaxed);
}

void SysVars::set_total_log_size(uint64_t size) noexcept {
  uint64_t current_size = total_log_size.load();
  while (!total_log_size.compare_exchange_weak(current_size, size)) {
  }
}

void SysVars::update_total_log_size(uint64_t size) noexcept {
  total_log_size.fetch_add(size, std::memory_order_relaxed);
}

void SysVars::inc_direct_writes() noexcept {
  direct_writes.fetch_add(1, std::memory_order_relaxed);
}

#ifndef NDEBUG
std::chrono::system_clock::time_point
SysVars::get_debug_time_point_for_rotation() noexcept {
  static auto debug_time_point = get_initial_debug_time_point();

  DBUG_EXECUTE_IF("audit_log_filter_reset_log_bookmark", {
    DBUG_SET("-d,audit_log_filter_reset_log_bookmark");
    debug_time_point = get_initial_debug_time_point();
    SysVars::update_log_bookmark(0, "");
    SysVars::init_record_id(0);
  });

  debug_time_point += std::chrono::minutes{1};
  return debug_time_point;
}

std::chrono::system_clock::time_point
SysVars::get_debug_time_point_for_encryption() noexcept {
  static auto debug_time_point = get_initial_debug_time_point();
  debug_time_point += std::chrono::hours{24};
  return debug_time_point;
}
#endif

uint64_t SysVars::get_next_record_id() noexcept {
  return record_id.fetch_add(1, std::memory_order_relaxed);
}

void SysVars::init_record_id(uint64_t initial_record_id) noexcept {
  record_id.store(initial_record_id);
}

void SysVars::set_encryption_options_id(
    const std::string &options_id) noexcept {
  encryption_options_id = options_id;
}

std::string SysVars::get_encryption_options_id() noexcept {
  return encryption_options_id;
}

comp_registry_srv_t *SysVars::acquire_comp_registry_srv() noexcept {
  assert(comp_registry_srv == nullptr);
  comp_registry_srv = get_component_registry_service();
  return comp_registry_srv.get();
}

void SysVars::release_comp_registry_srv() noexcept {
  assert(comp_registry_srv != nullptr);
  comp_registry_srv.reset();
}

comp_registry_srv_t *SysVars::get_comp_registry_srv() noexcept {
  assert(comp_registry_srv != nullptr);
  return comp_registry_srv.get();
}

}  // namespace audit_log_filter
