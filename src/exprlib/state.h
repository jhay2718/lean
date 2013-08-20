/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "output_channel.h"
#include "formatter.h"
#include "options.h"

namespace lean {
/**
   \brief State provided to internal lean procedures that need to:
   1- Access user defined options
   2- Produce verbosity messages
   3- Output results
   4- Produce formatted output
*/
class state {
    options                         m_options;
    formatter                       m_formatter;
    std::shared_ptr<output_channel> m_regular_channel;
    std::shared_ptr<output_channel> m_diagnostic_channel;
public:
    state();
    state(options const & opts, formatter const & fmt);
    ~state();

    options get_options() const { return m_options; }
    formatter get_formatter() const { return m_formatter; }
    output_channel & get_regular_channel() const { return *m_regular_channel; }
    output_channel & get_diagnostic_channel() const { return *m_diagnostic_channel; }

    void set_regular_channel(std::shared_ptr<output_channel> const & out);
    void set_diagnostic_channel(std::shared_ptr<output_channel> const & out);
    void set_options(options const & opts);
    void set_formatter(formatter const & f);
};

struct regular {
    state const & m_state;
    regular(state const & s):m_state(s) {}
};

struct diagnostic {
    state const & m_state;
    diagnostic(state const & s):m_state(s) {}
};

template<typename T>
inline regular const & operator<<(regular const & out, T const & t) {
    out.m_state.get_regular_channel().get_stream() << t;
    return out;
}

template<typename T>
inline diagnostic const & operator<<(diagnostic const & out, T const & t) {
    out.m_state.get_diagnostic_channel().get_stream() << t;
    return out;
}

inline regular const & operator<<(regular const & out, expr const & e) {
    out.m_state.get_regular_channel().get_stream() << out.m_state.get_formatter()(e, out.m_state.get_options());
    return out;
}

inline diagnostic const & operator<<(diagnostic const & out, expr const & e) {
    out.m_state.get_diagnostic_channel().get_stream() << out.m_state.get_formatter()(e, out.m_state.get_options());
    return out;
}
}
