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

#ifndef AUDIT_LOG_FILTER_EVENT_FILTER_FUNCTION_STRING_FIND_H_INCLUDED
#define AUDIT_LOG_FILTER_EVENT_FILTER_FUNCTION_STRING_FIND_H_INCLUDED

#include "components/audit_log_filter/event_filter_function/base.h"

namespace audit_log_filter::event_filter_function {

template <>
class EventFilterFunction<EventFilterFunctionType::StringFind>
    : public EventFilterFunctionBase {
 public:
  explicit EventFilterFunction(FunctionArgsList args);

  /**
   * @brief Validate function arguments.
   *
   * @param args Arguments list
   * @param expected_return_type Expected function return type
   * @return true in case arguments are valid, false otherwise
   */
  static bool validate_args(const FunctionArgsList &args,
                            FunctionReturnType expected_return_type) noexcept;

  using EventFilterFunctionBase::exec;

  /**
   * @brief Execute function in boolean context.
   *
   * @param fields Audit event fields list
   * @param result Function result
   * @return true in case function executed successfully, false otherwise
   */
  bool exec(const AuditRecordFieldsList &fields,
            bool &result) noexcept override;
};

using EventFilterFunctionStringFind =
    EventFilterFunction<EventFilterFunctionType::StringFind>;

}  // namespace audit_log_filter::event_filter_function

#endif  // AUDIT_LOG_FILTER_EVENT_FILTER_FUNCTION_STRING_FIND_H_INCLUDED
