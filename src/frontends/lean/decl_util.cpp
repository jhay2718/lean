/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <algorithm>
#include "kernel/instantiate.h"
#include "kernel/abstract.h"
#include "kernel/for_each_fn.h"
#include "library/locals.h"
#include "library/placeholder.h"
#include "library/protected.h"
#include "library/aliases.h"
#include "library/scoped_ext.h"
#include "library/tactic/elaborate.h"
#include "frontends/lean/util.h"
#include "frontends/lean/decl_util.h"
#include "frontends/lean/tokens.h"
#include "frontends/lean/decl_attributes.h"
#include "frontends/lean/parser.h"
#include "frontends/lean/elaborator.h"

namespace lean {
bool parse_univ_params(parser & p, buffer<name> & lp_names) {
    if (p.curr_is_token(get_llevel_curly_tk())) {
        p.next();
        while (!p.curr_is_token(get_rcurly_tk())) {
            name l = p.check_id_next("invalid universe parameter, identifier expected");
            p.add_local_level(l, mk_param_univ(l));
            lp_names.push_back(l);
        }
        p.next();
        return true;
    } else{
        return false;
    }
}

expr parse_single_header(parser & p, buffer<name> & lp_names, buffer<expr> & params, bool is_example) {
    auto c_pos  = p.pos();
    name c_name;
    if (is_example)
        c_name = name("this");
    else
        c_name = p.check_decl_id_next("invalid declaration, identifier expected");
    declaration_name_scope scope(c_name);
    parse_univ_params(p, lp_names);
    p.parse_optional_binders(params);
    for (expr const & param : params)
        p.add_local(param);
    expr type;
    if (p.curr_is_token(get_colon_tk())) {
        p.next();
        type = p.parse_expr();
    } else {
        type = p.save_pos(mk_expr_placeholder(), c_pos);
    }
    expr c   = p.save_pos(mk_local(c_name, type), c_pos);
    return c;
}

void parse_mutual_header(parser & p, buffer<name> & lp_names, buffer<expr> & cs, buffer<expr> & params) {
    if (p.curr_is_token(get_lcurly_tk())) {
        p.next();
        while (!p.curr_is_token(get_rcurly_tk())) {
            name l = p.check_atomic_id_next("invalid mutual declaration, identifier expected");
            lp_names.push_back(l);
            p.add_local_level(l, mk_param_univ(l));
        }
        p.next();
    }
    while (true) {
        auto c_pos  = p.pos();
        name c_name = p.check_decl_id_next("invalid mutual declaration, identifier expected");
        cs.push_back(p.save_pos(mk_local(c_name, mk_expr_placeholder()), c_pos));
        if (!p.curr_is_token(get_comma_tk()))
            break;
        p.next();
    }
    p.parse_optional_binders(params);
    for (expr const & param : params)
        p.add_local(param);
    for (expr const & c : cs)
        p.add_local(c);
}

pair<expr, decl_attributes> parse_inner_header(parser & p, name const & c_expected) {
    decl_attributes attrs;
    p.check_token_next(get_with_tk(), "invalid mutual declaration, 'with' expected");
    attrs.parse(p);
    auto id_pos = p.pos();
    name n = p.check_decl_id_next("invalid mutual declaration, identifier expected");
    if (c_expected != n)
        throw parser_error(sstream() << "invalid mutual declaration, '" << c_expected << "' expected",
                           id_pos);
    declaration_name_scope scope(n);
    p.check_token_next(get_colon_tk(), "invalid mutual declaration, ':' expected");
    return mk_pair(p.parse_expr(), attrs);
}

/** \brief Version of collect_locals(expr const & e, collected_locals & ls) that ignores local constants occurring in
    tactics. */
void collect_locals_ignoring_tactics(expr const & e, collected_locals & ls) {
    if (!has_local(e)) return;
    for_each(e, [&](expr const & e, unsigned) {
            if (!has_local(e)) return false;
            if (is_by(e))      return false; // do not visit children
            if (is_local(e))   ls.insert(e);
            return true;
        });
}

name_set collect_univ_params_ignoring_tactics(expr const & e, name_set const & ls) {
    if (!has_param_univ(e)) return ls;
    name_set r = ls;
    for_each(e, [&](expr const & e, unsigned) {
            if (!has_param_univ(e)) {
                return false;
            } else if (is_by(e)) {
                return false;
            } else if (is_sort(e)) {
                collect_univ_params_core(sort_level(e), r);
            } else if (is_constant(e)) {
                for (auto const & l : const_levels(e))
                    collect_univ_params_core(l, r);
            }
            return true;
        });
    return r;
}

/** \brief Collect annonymous instances in section/namespace declarations such as:

        variable [decidable_eq A]
*/
void collect_annonymous_inst_implicit(parser const & p, collected_locals & locals) {
    buffer<pair<name, expr>> entries;
    to_buffer(p.get_local_entries(), entries);
    unsigned i = entries.size();
    while (i > 0) {
        --i;
        auto const & entry = entries[i];
        if (is_local(entry.second) && !locals.contains(entry.second) && local_info(entry.second).is_inst_implicit() &&
            // remark: remove the following condition condition, if we want to auto inclusion also for non anonymous ones.
            p.is_anonymous_inst_name(entry.first)) {
            bool ok = true;
            for_each(mlocal_type(entry.second), [&](expr const & e, unsigned) {
                    if (!ok) return false; // stop
                    if (is_local(e) && !locals.contains(e))
                        ok = false;
                    return true;
                });
            if (ok)
                locals.insert(entry.second);
        }
    }
}

/** \brief Sort local names by order of occurrence, and copy the associated parameters to ps */
void sort_locals(buffer<expr> const & locals, parser const & p, buffer<expr> & ps) {
    buffer<expr> extra;
    name_set     explicit_param_names;
    for (expr const & p : ps) {
        explicit_param_names.insert(mlocal_name(p));
    }
    for (expr const & l : locals) {
        // we only copy the locals that are in p's local context
        if (p.is_local_decl(l) && !explicit_param_names.contains(mlocal_name(l)))
            extra.push_back(l);
    }
    std::sort(extra.begin(), extra.end(), [&](expr const & p1, expr const & p2) {
            bool is_var1 = p.is_local_variable(p1);
            bool is_var2 = p.is_local_variable(p2);
            if (!is_var1 && is_var2)
                return true;
            else if (is_var1 && !is_var2)
                return false;
            else
                return p.get_local_index(p1) < p.get_local_index(p2);
        });
    buffer<expr> new_ps;
    new_ps.append(extra);
    new_ps.append(ps);
    ps.clear();
    ps.append(new_ps);
}

/** TODO(Leo): mark as static */
void update_univ_parameters(parser & p, buffer<name> & lp_names, name_set const & found) {
    unsigned old_sz = lp_names.size();
    found.for_each([&](name const & n) {
            if (std::find(lp_names.begin(), lp_names.begin() + old_sz, n) == lp_names.begin() + old_sz)
                lp_names.push_back(n);
        });
    std::sort(lp_names.begin(), lp_names.end(), [&](name const & n1, name const & n2) {
            return p.get_local_level_index(n1) < p.get_local_level_index(n2);
        });
}

void collect_implicit_locals(parser & p, buffer<name> & lp_names, buffer<expr> & params, buffer<expr> const & all_exprs) {
    collected_locals locals;
    buffer<expr> include_vars;
    name_set lp_found;
    /** Process variables included using the 'include' command */
    p.get_include_variables(include_vars);
    for (expr const & param : include_vars) {
        if (is_local(param)) {
            collect_locals_ignoring_tactics(mlocal_type(param), locals);
            lp_found = collect_univ_params_ignoring_tactics(mlocal_type(param), lp_found);
            locals.insert(param);
        }
    }
    /** Process explicit parameters */
    for (expr const & param : params) {
        collect_locals_ignoring_tactics(mlocal_type(param), locals);
        lp_found = collect_univ_params_ignoring_tactics(mlocal_type(param), lp_found);
        locals.insert(param);
    }
    /** Process expressions used to define declaration. */
    for (expr const & e : all_exprs) {
        collect_locals_ignoring_tactics(e, locals);
        lp_found = collect_univ_params_ignoring_tactics(e, lp_found);
    }
    collect_annonymous_inst_implicit(p, locals);
    sort_locals(locals.get_collected(), p, params);
    update_univ_parameters(p, lp_names, lp_found);
}

void collect_implicit_locals(parser & p, buffer<name> & lp_names, buffer<expr> & params, std::initializer_list<expr> const & all_exprs) {
    buffer<expr> tmp; tmp.append(all_exprs.size(), all_exprs.begin());
    collect_implicit_locals(p, lp_names, params, tmp);
}

void collect_implicit_locals(parser & p, buffer<name> & lp_names, buffer<expr> & params, expr const & e) {
    buffer<expr> all_exprs; all_exprs.push_back(e);
    collect_implicit_locals(p, lp_names, params, all_exprs);
}

void elaborate_params(elaborator & elab, buffer<expr> const & params, buffer<expr> & new_params) {
    for (unsigned i = 0; i < params.size(); i++) {
        expr const & param = params[i];
        expr type          = replace_locals(mlocal_type(param), i, params.data(), new_params.data());
        expr new_type      = elab.elaborate_type(type);
        expr new_param     = elab.push_local(local_pp_name(param), new_type, local_info(param));
        new_params.push_back(new_param);
    }
}

environment add_local_ref(parser & p, environment const & env, name const & c_name, name const & c_real_name, buffer<name> const & lp_names, buffer<expr> const & var_params) {
    if (!p.has_params()) return env;
    buffer<expr> params;
    buffer<name> lps;
    for (name const & u : lp_names) {
        if (p.is_local_level_variable(u))
            break;
        lps.push_back(u);
    }
    for (expr const & e : var_params) {
        if (p.is_local_variable(e))
            break;
        params.push_back(e);
    }
    if (lps.empty() && params.empty()) return env;
    expr ref = mk_local_ref(c_real_name, param_names_to_levels(to_list(lps)), params);
    return p.add_local_ref(env, c_name, ref);
}

environment add_alias(environment const & env, bool is_protected, name const & c_name, name const & c_real_name) {
    if (c_name != c_real_name) {
        if (is_protected)
            return add_expr_alias_rec(env, get_protected_shortest_name(c_real_name), c_real_name);
        else
            return add_expr_alias_rec(env, c_name, c_real_name);
    } else {
        return env;
    }
}

struct definition_info {
    name     m_prefix;
    bool     m_is_private{false};
    bool     m_is_meta{false};
    bool     m_is_noncomputable{false};
    bool     m_is_lemma{false};
    bool     m_aux_lemmas{false};
    unsigned m_next_match_idx{1};
};

MK_THREAD_LOCAL_GET_DEF(definition_info, get_definition_info);

declaration_info_scope::declaration_info_scope(environment const & env, bool is_private, bool is_meta,
                                               bool is_noncomputable, bool is_lemma, bool aux_lemmas) {
    definition_info & info = get_definition_info();
    lean_assert(info.m_prefix.is_anonymous());
    info.m_prefix           = is_private ? name() : get_namespace(env);
    info.m_is_private       = is_private;
    info.m_is_meta          = is_meta;
    info.m_is_noncomputable = is_noncomputable;
    info.m_is_lemma         = is_lemma;
    info.m_aux_lemmas       = aux_lemmas;
    info.m_next_match_idx = 1;
}

declaration_info_scope::declaration_info_scope(environment const & env, bool is_private, bool is_noncomputable, def_cmd_kind k):
    declaration_info_scope(env, is_private, k == MetaDefinition, is_noncomputable, k == Theorem, k == Definition) {}

declaration_info_scope::~declaration_info_scope() {
    definition_info & info = get_definition_info();
    info.m_prefix = name();
}

equations_header mk_equations_header(list<name> const & ns) {
    equations_header h;
    h.m_num_fns          = length(ns);
    h.m_fn_names         = ns;
    h.m_is_private       = get_definition_info().m_is_private;
    h.m_is_meta          = get_definition_info().m_is_meta;
    h.m_is_noncomputable = get_definition_info().m_is_noncomputable;
    h.m_is_lemma         = get_definition_info().m_is_lemma;
    h.m_aux_lemmas       = get_definition_info().m_aux_lemmas;
    return h;
}

equations_header mk_equations_header(name const & n) {
    return mk_equations_header(to_list(n));
}

/* Auxiliary function for creating names for auxiliary declarations.
   We avoid propagating the suffix `_main` used by the top-level equations
   to the nested declarations. */
static name mk_decl_name(name const & prefix, name const & n) {
    if (!prefix.is_atomic() && prefix.is_string() && strcmp(prefix.get_string(), "_main") == 0) {
        return prefix.get_prefix() + n;
    } else {
        return prefix + n;
    }
}

declaration_name_scope::declaration_name_scope(name const & n) {
    definition_info & info = get_definition_info();
    m_old_prefix          = info.m_prefix;
    m_old_next_match_idx  = info.m_next_match_idx;
    info.m_prefix         = mk_decl_name(info.m_prefix, n);
    info.m_next_match_idx = 1;
    m_name                = info.m_prefix;
}

declaration_name_scope::~declaration_name_scope() {
    definition_info & info = get_definition_info();
    info.m_prefix          = m_old_prefix;
    info.m_next_match_idx  = m_old_next_match_idx;
}

match_definition_scope::match_definition_scope() {
    definition_info & info = get_definition_info();
    m_name = mk_decl_name(info.m_prefix, name("_match").append_after(info.m_next_match_idx));
    info.m_next_match_idx++;
}
}