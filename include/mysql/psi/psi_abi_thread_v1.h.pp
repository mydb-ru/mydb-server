#include "mysql/psi/psi_thread.h"
#include "my_inttypes.h"
#include "my_config.h"
typedef unsigned char uchar;
typedef long long int longlong;
typedef unsigned long long int ulonglong;
typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef intptr_t intptr;
typedef ulonglong my_off_t;
typedef int myf;
#include "my_macros.h"
#include "my_psi_config.h"
#include "my_sharedlib.h"
#include "mysql/components/services/bits/psi_thread_bits.h"
#include <mysql/components/services/bits/my_io_bits.h>
typedef int File;
typedef mode_t MY_MODE;
typedef socklen_t socket_len_t;
typedef int my_socket;
#include <mysql/components/services/bits/my_thread_bits.h>
typedef pthread_t my_thread_t;
typedef pthread_attr_t my_thread_attr_t;
static constexpr my_thread_t null_thread_initializer = my_thread_t{};
struct my_thread_handle {
  my_thread_t thread{null_thread_initializer};
};
#include <mysql/components/services/bits/psi_bits.h>
static constexpr unsigned PSI_INSTRUMENT_ME = 0;
static constexpr unsigned PSI_NOT_INSTRUMENTED = 0;
struct PSI_placeholder {
  int m_placeholder;
};
struct PSI_instr {
  bool m_enabled;
};
typedef unsigned int PSI_thread_key;
typedef unsigned int PSI_thread_seqnum;
struct opaque_THD {
  int dummy;
};
typedef struct opaque_THD THD;
typedef int opaque_vio_type;
struct PSI_thread;
typedef struct PSI_thread PSI_thread;
struct PSI_thread_info_v1 {
  PSI_thread_key *m_key;
  const char *m_name;
  unsigned int m_flags;
  int m_volatility;
  const char *m_documentation;
};
typedef struct PSI_thread_info_v1 PSI_thread_info_v1;
struct PSI_thread_info_v5 {
  PSI_thread_key *m_key;
  const char *m_name;
  const char *m_os_name;
  unsigned int m_flags;
  int m_volatility;
  const char *m_documentation;
};
typedef struct PSI_thread_info_v5 PSI_thread_info_v5;
typedef void (*register_thread_v1_t)(const char *category,
                                     struct PSI_thread_info_v1 *info,
                                     int count);
typedef void (*register_thread_v5_t)(const char *category,
                                     struct PSI_thread_info_v5 *info,
                                     int count);
typedef int (*spawn_thread_v1_t)(PSI_thread_key key, my_thread_handle *thread,
                                 const my_thread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg);
typedef int (*spawn_thread_v5_t)(PSI_thread_key key, PSI_thread_seqnum seqnum,
                                 my_thread_handle *thread,
                                 const my_thread_attr_t *attr,
                                 void *(*start_routine)(void *), void *arg);
typedef struct PSI_thread *(*new_thread_v1_t)(PSI_thread_key key,
                                              const void *identity,
                                              unsigned long long thread_id);
typedef struct PSI_thread *(*new_thread_v5_t)(PSI_thread_key key,
                                              PSI_thread_seqnum seqnum,
                                              const void *identity,
                                              unsigned long long thread_id);
typedef void (*set_thread_THD_v1_t)(struct PSI_thread *thread, THD *thd);
typedef void (*set_thread_id_v1_t)(struct PSI_thread *thread,
                                   unsigned long long id);
typedef unsigned long long (*get_current_thread_internal_id_v2_t)();
typedef unsigned long long (*get_thread_internal_id_v2_t)(
    struct PSI_thread *thread);
typedef struct PSI_thread *(*get_thread_by_id_v2_t)(
    unsigned long long processlist_id);
typedef void (*set_thread_os_id_v1_t)(struct PSI_thread *thread);
typedef struct PSI_thread *(*get_thread_v1_t)(void);
typedef void (*set_thread_user_v1_t)(const char *user, int user_len);
typedef void (*set_thread_account_v1_t)(const char *user, int user_len,
                                        const char *host, int host_len);
typedef void (*set_thread_db_v1_t)(const char *db, int db_len);
typedef void (*set_thread_command_v1_t)(int command);
typedef void (*set_connection_type_v1_t)(opaque_vio_type conn_type);
typedef void (*set_thread_start_time_v1_t)(time_t start_time);
typedef void (*set_thread_start_time_usec_v4_t)(
    unsigned long long start_time_usec);
typedef void (*set_thread_rows_sent_v4_t)(unsigned long long rows_sent);
typedef void (*set_thread_rows_examined_v4_t)(unsigned long long rows_examined);
typedef void (*set_thread_state_v1_t)(const char *state);
typedef void (*set_thread_info_v1_t)(const char *info, unsigned int info_len);
typedef void (*set_thread_secondary_engine_v6_t)(bool secondary);
typedef int (*set_thread_resource_group_v1_t)(const char *group_name,
                                              int group_name_len,
                                              void *user_data);
typedef int (*set_thread_resource_group_by_id_v1_t)(
    PSI_thread *thread, unsigned long long thread_id, const char *group_name,
    int group_name_len, void *user_data);
typedef void (*set_thread_v1_t)(struct PSI_thread *thread);
typedef void (*set_thread_peer_port_v4_t)(PSI_thread *thread,
                                          unsigned int port);
typedef void (*aggregate_thread_status_v2_t)(struct PSI_thread *thread);
typedef void (*delete_current_thread_v1_t)(void);
typedef void (*delete_thread_v1_t)(struct PSI_thread *thread);
typedef int (*set_thread_connect_attrs_v1_t)(const char *buffer,
                                             unsigned int length,
                                             const void *from_cs);
typedef void (*get_current_thread_event_id_v2_t)(
    unsigned long long *thread_internal_id, unsigned long long *event_id);
typedef void (*get_thread_event_id_v1_t)(unsigned long long *thread_internal_id,
                                         unsigned long long *event_id);
typedef void (*get_thread_event_id_v2_t)(struct PSI_thread *psi,
                                         unsigned long long *thread_internal_id,
                                         unsigned long long *event_id);
struct PSI_thread_attrs_v3 {
  unsigned long long m_thread_internal_id;
  unsigned long m_processlist_id;
  unsigned long long m_thread_os_id;
  void *m_user_data;
  char m_username[(32 * 3)];
  size_t m_username_length;
  char m_hostname[(255)];
  size_t m_hostname_length;
  char m_groupname[(64 * 3)];
  size_t m_groupname_length;
  struct sockaddr_storage m_sock_addr;
  socklen_t m_sock_addr_length;
  bool m_system_thread;
};
typedef struct PSI_thread_attrs_v3 PSI_thread_attrs;
typedef void (*PSI_notification_cb_v3)(const PSI_thread_attrs_v3 *thread_attrs);
struct PSI_notification_v3 {
  PSI_notification_cb_v3 thread_create;
  PSI_notification_cb_v3 thread_destroy;
  PSI_notification_cb_v3 session_connect;
  PSI_notification_cb_v3 session_disconnect;
  PSI_notification_cb_v3 session_change_user;
};
typedef struct PSI_notification_v3 PSI_notification;
typedef int (*get_thread_system_attrs_v3_t)(PSI_thread_attrs_v3 *thread_attrs);
typedef int (*get_thread_system_attrs_by_id_v3_t)(
    PSI_thread *thread, unsigned long long thread_id,
    PSI_thread_attrs_v3 *thread_attrs);
typedef int (*register_notification_v3_t)(const PSI_notification_v3 *callbacks,
                                          bool with_ref_count);
typedef int (*unregister_notification_v1_t)(int handle);
typedef void (*notify_session_connect_v1_t)(PSI_thread *thread);
typedef void (*notify_session_disconnect_v1_t)(PSI_thread *thread);
typedef void (*notify_session_change_user_v1_t)(PSI_thread *thread);
typedef struct PSI_thread_info_v5 PSI_thread_info;
typedef void (*thread_detect_telemetry_v7_t)(PSI_thread *thread);
typedef void (*thread_abort_telemetry_v7_t)(PSI_thread *thread);
struct PSI_thread_bootstrap {
  void *(*get_interface)(int version);
};
typedef struct PSI_thread_bootstrap PSI_thread_bootstrap;
typedef void (*set_mem_cnt_THD_v1_t)(THD *thd, THD **backup_thd);
struct PSI_thread_service_v4 {
  register_thread_v1_t register_thread;
  spawn_thread_v1_t spawn_thread;
  new_thread_v1_t new_thread;
  set_thread_id_v1_t set_thread_id;
  get_current_thread_internal_id_v2_t get_current_thread_internal_id;
  get_thread_internal_id_v2_t get_thread_internal_id;
  get_thread_by_id_v2_t get_thread_by_id;
  set_thread_THD_v1_t set_thread_THD;
  set_thread_os_id_v1_t set_thread_os_id;
  get_thread_v1_t get_thread;
  set_thread_user_v1_t set_thread_user;
  set_thread_account_v1_t set_thread_account;
  set_thread_db_v1_t set_thread_db;
  set_thread_command_v1_t set_thread_command;
  set_connection_type_v1_t set_connection_type;
  set_thread_start_time_v1_t set_thread_start_time;
  set_thread_start_time_usec_v4_t set_thread_start_time_usec;
  set_thread_rows_sent_v4_t set_thread_rows_sent;
  set_thread_rows_examined_v4_t set_thread_rows_examined;
  set_thread_info_v1_t set_thread_info;
  set_thread_resource_group_v1_t set_thread_resource_group;
  set_thread_resource_group_by_id_v1_t set_thread_resource_group_by_id;
  set_thread_v1_t set_thread;
  set_thread_peer_port_v4_t set_thread_peer_port;
  aggregate_thread_status_v2_t aggregate_thread_status;
  delete_current_thread_v1_t delete_current_thread;
  delete_thread_v1_t delete_thread;
  set_thread_connect_attrs_v1_t set_thread_connect_attrs;
  get_current_thread_event_id_v2_t get_current_thread_event_id;
  get_thread_event_id_v2_t get_thread_event_id;
  get_thread_system_attrs_v3_t get_thread_system_attrs;
  get_thread_system_attrs_by_id_v3_t get_thread_system_attrs_by_id;
  register_notification_v3_t register_notification;
  unregister_notification_v1_t unregister_notification;
  notify_session_connect_v1_t notify_session_connect;
  notify_session_disconnect_v1_t notify_session_disconnect;
  notify_session_change_user_v1_t notify_session_change_user;
};
struct PSI_thread_service_v5 {
  register_thread_v5_t register_thread;
  spawn_thread_v5_t spawn_thread;
  new_thread_v5_t new_thread;
  set_thread_id_v1_t set_thread_id;
  get_current_thread_internal_id_v2_t get_current_thread_internal_id;
  get_thread_internal_id_v2_t get_thread_internal_id;
  get_thread_by_id_v2_t get_thread_by_id;
  set_thread_THD_v1_t set_thread_THD;
  set_thread_os_id_v1_t set_thread_os_id;
  get_thread_v1_t get_thread;
  set_thread_user_v1_t set_thread_user;
  set_thread_account_v1_t set_thread_account;
  set_thread_db_v1_t set_thread_db;
  set_thread_command_v1_t set_thread_command;
  set_connection_type_v1_t set_connection_type;
  set_thread_start_time_v1_t set_thread_start_time;
  set_thread_start_time_usec_v4_t set_thread_start_time_usec;
  set_thread_rows_sent_v4_t set_thread_rows_sent;
  set_thread_rows_examined_v4_t set_thread_rows_examined;
  set_thread_info_v1_t set_thread_info;
  set_thread_resource_group_v1_t set_thread_resource_group;
  set_thread_resource_group_by_id_v1_t set_thread_resource_group_by_id;
  set_thread_v1_t set_thread;
  set_thread_peer_port_v4_t set_thread_peer_port;
  aggregate_thread_status_v2_t aggregate_thread_status;
  delete_current_thread_v1_t delete_current_thread;
  delete_thread_v1_t delete_thread;
  set_thread_connect_attrs_v1_t set_thread_connect_attrs;
  get_current_thread_event_id_v2_t get_current_thread_event_id;
  get_thread_event_id_v2_t get_thread_event_id;
  get_thread_system_attrs_v3_t get_thread_system_attrs;
  get_thread_system_attrs_by_id_v3_t get_thread_system_attrs_by_id;
  register_notification_v3_t register_notification;
  unregister_notification_v1_t unregister_notification;
  notify_session_connect_v1_t notify_session_connect;
  notify_session_disconnect_v1_t notify_session_disconnect;
  notify_session_change_user_v1_t notify_session_change_user;
  set_mem_cnt_THD_v1_t set_mem_cnt_THD;
};
struct PSI_thread_service_v6 {
  register_thread_v5_t register_thread;
  spawn_thread_v5_t spawn_thread;
  new_thread_v5_t new_thread;
  set_thread_id_v1_t set_thread_id;
  get_current_thread_internal_id_v2_t get_current_thread_internal_id;
  get_thread_internal_id_v2_t get_thread_internal_id;
  get_thread_by_id_v2_t get_thread_by_id;
  set_thread_THD_v1_t set_thread_THD;
  set_thread_os_id_v1_t set_thread_os_id;
  get_thread_v1_t get_thread;
  set_thread_user_v1_t set_thread_user;
  set_thread_account_v1_t set_thread_account;
  set_thread_db_v1_t set_thread_db;
  set_thread_command_v1_t set_thread_command;
  set_connection_type_v1_t set_connection_type;
  set_thread_start_time_v1_t set_thread_start_time;
  set_thread_start_time_usec_v4_t set_thread_start_time_usec;
  set_thread_rows_sent_v4_t set_thread_rows_sent;
  set_thread_rows_examined_v4_t set_thread_rows_examined;
  set_thread_info_v1_t set_thread_info;
  set_thread_secondary_engine_v6_t set_thread_secondary_engine;
  set_thread_resource_group_v1_t set_thread_resource_group;
  set_thread_resource_group_by_id_v1_t set_thread_resource_group_by_id;
  set_thread_v1_t set_thread;
  set_thread_peer_port_v4_t set_thread_peer_port;
  aggregate_thread_status_v2_t aggregate_thread_status;
  delete_current_thread_v1_t delete_current_thread;
  delete_thread_v1_t delete_thread;
  set_thread_connect_attrs_v1_t set_thread_connect_attrs;
  get_current_thread_event_id_v2_t get_current_thread_event_id;
  get_thread_event_id_v2_t get_thread_event_id;
  get_thread_system_attrs_v3_t get_thread_system_attrs;
  get_thread_system_attrs_by_id_v3_t get_thread_system_attrs_by_id;
  register_notification_v3_t register_notification;
  unregister_notification_v1_t unregister_notification;
  notify_session_connect_v1_t notify_session_connect;
  notify_session_disconnect_v1_t notify_session_disconnect;
  notify_session_change_user_v1_t notify_session_change_user;
  set_mem_cnt_THD_v1_t set_mem_cnt_THD;
};
struct PSI_thread_service_v7 {
  register_thread_v5_t register_thread;
  spawn_thread_v5_t spawn_thread;
  new_thread_v5_t new_thread;
  set_thread_id_v1_t set_thread_id;
  get_current_thread_internal_id_v2_t get_current_thread_internal_id;
  get_thread_internal_id_v2_t get_thread_internal_id;
  get_thread_by_id_v2_t get_thread_by_id;
  set_thread_THD_v1_t set_thread_THD;
  set_thread_os_id_v1_t set_thread_os_id;
  get_thread_v1_t get_thread;
  set_thread_user_v1_t set_thread_user;
  set_thread_account_v1_t set_thread_account;
  set_thread_db_v1_t set_thread_db;
  set_thread_command_v1_t set_thread_command;
  set_connection_type_v1_t set_connection_type;
  set_thread_start_time_v1_t set_thread_start_time;
  set_thread_start_time_usec_v4_t set_thread_start_time_usec;
  set_thread_rows_sent_v4_t set_thread_rows_sent;
  set_thread_rows_examined_v4_t set_thread_rows_examined;
  set_thread_info_v1_t set_thread_info;
  set_thread_secondary_engine_v6_t set_thread_secondary_engine;
  set_thread_resource_group_v1_t set_thread_resource_group;
  set_thread_resource_group_by_id_v1_t set_thread_resource_group_by_id;
  set_thread_v1_t set_thread;
  set_thread_peer_port_v4_t set_thread_peer_port;
  aggregate_thread_status_v2_t aggregate_thread_status;
  delete_current_thread_v1_t delete_current_thread;
  delete_thread_v1_t delete_thread;
  set_thread_connect_attrs_v1_t set_thread_connect_attrs;
  get_current_thread_event_id_v2_t get_current_thread_event_id;
  get_thread_event_id_v2_t get_thread_event_id;
  get_thread_system_attrs_v3_t get_thread_system_attrs;
  get_thread_system_attrs_by_id_v3_t get_thread_system_attrs_by_id;
  register_notification_v3_t register_notification;
  unregister_notification_v1_t unregister_notification;
  notify_session_connect_v1_t notify_session_connect;
  notify_session_disconnect_v1_t notify_session_disconnect;
  notify_session_change_user_v1_t notify_session_change_user;
  set_mem_cnt_THD_v1_t set_mem_cnt_THD;
  thread_detect_telemetry_v7_t detect_telemetry;
  thread_abort_telemetry_v7_t abort_telemetry;
};
typedef struct PSI_thread_service_v7 PSI_thread_service_t;
extern PSI_thread_service_t *psi_thread_service;
