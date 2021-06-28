/* Copyright (c) 2000, 2021, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SQL_RANGE_OPTIMIZER_INDEX_MERGE_PLAN_H_
#define SQL_RANGE_OPTIMIZER_INDEX_MERGE_PLAN_H_

#include "sql/range_optimizer/table_read_plan.h"

class Opt_trace_object;
class PARAM;
class QUICK_SELECT_I;
class TRP_RANGE;
struct MEM_ROOT;

/*
  Plan for QUICK_INDEX_MERGE_SELECT scan.
  QUICK_ROR_INTERSECT_SELECT always retrieves full rows, so retrieve_full_rows
  is ignored by make_quick.
*/

class TRP_INDEX_MERGE : public TABLE_READ_PLAN {
  bool forced_by_hint;

 public:
  explicit TRP_INDEX_MERGE(bool forced_by_hint_arg)
      : forced_by_hint(forced_by_hint_arg) {}
  QUICK_SELECT_I *make_quick(PARAM *param, bool retrieve_full_rows,
                             MEM_ROOT *parent_alloc) override;
  TRP_RANGE **range_scans;     /* array of ptrs to plans of merged scans */
  TRP_RANGE **range_scans_end; /* end of the array */

  void trace_basic_info(const PARAM *param,
                        Opt_trace_object *trace_object) const override;
};

#endif  // SQL_RANGE_OPTIMIZER_INDEX_MERGE_PLAN_H_