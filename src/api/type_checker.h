/*
Copyright (c) 2015 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "kernel/declaration.h"
#include "kernel/type_checker.h"
#include "api/expr.h"
#include "api/decl.h"
#include "api/lean_type_checker.h"
namespace lean {
inline type_checker * to_type_checker(lean_type_checker n) { return reinterpret_cast<type_checker *>(n); }
inline type_checker & to_type_checker_ref(lean_type_checker n) { return *reinterpret_cast<type_checker *>(n); }
inline lean_type_checker of_type_checker(type_checker * n) { return reinterpret_cast<lean_type_checker>(n); }

inline constraint_seq * to_cnstr_seq(lean_cnstr_seq n) { return reinterpret_cast<constraint_seq *>(n); }
inline constraint_seq const & to_cnstr_seq_ref(lean_type_checker n) { return *reinterpret_cast<constraint_seq *>(n); }
inline lean_cnstr_seq of_cnstr_seq(constraint_seq * n) { return reinterpret_cast<lean_cnstr_seq>(n); }
}
