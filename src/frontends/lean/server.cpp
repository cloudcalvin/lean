/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include <string>
#include <functional>
#include "util/sstream.h"
#include "frontends/lean/server.h"
#include "frontends/lean/parser.h"

namespace lean {
server::file::file(std::string const & fname):m_fname(fname), m_from(0) {}

void server::file::replace_line(unsigned linenum, std::string const & new_line) {
    while (linenum >= m_lines.size())
        m_lines.push_back("");
    m_lines[linenum] = new_line;
    if (linenum < m_from)
        m_from = linenum;
}

void server::file::insert_line(unsigned linenum, std::string const & new_line) {
    while (linenum >= m_lines.size())
        m_lines.push_back("");
    m_lines.push_back("");
    lean_assert(m_lines.size() >= linenum+1);
    unsigned i = m_lines.size();
    while (i > linenum) {
        --i;
        m_lines[i] = m_lines[i-1];
    }
    m_lines[linenum] = new_line;
    if (linenum < m_from)
        m_from = linenum;
}

void server::file::remove_line(unsigned linenum) {
    if (linenum >= m_lines.size())
        return;
    lean_assert(!m_lines.empty());
    for (unsigned i = linenum; i < m_lines.size()-1; i++)
        m_lines[i] = m_lines[i+1];
    m_lines.pop_back();
    if (linenum < m_from)
        m_from = linenum;
}

/**
   \brief Return index i <= m_snapshots.size() s.t.
      * forall j < i, m_snapshots[j].m_line < line
      * forall i <= j < m_snapshots.size(),  m_snapshots[j].m_line >= line
*/
unsigned server::file::find(unsigned linenum) {
    unsigned low  = 0;
    unsigned high = m_snapshots.size();
    while (true) {
        lean_assert(low <= high);
        if (low == high)
            return low;
        unsigned mid = low + ((high - low)/2);
        lean_assert(low <= mid && mid < high);
        lean_assert(mid < m_snapshots.size());
        snapshot const & s = m_snapshots[mid];
        if (s.m_line < linenum) {
            low  = mid+1;
        } else {
            high = mid;
        }
    }
}

server::server(environment const & env, io_state const & ios, unsigned num_threads):
    m_env(env), m_options(ios.get_options()), m_ios(ios), m_out(ios.get_regular_channel().get_stream()),
    m_num_threads(num_threads), m_empty_snapshot(m_env, m_options) {
}

static std::string g_load("LOAD");
static std::string g_visit("VISIT");
static std::string g_replace("REPLACE");
static std::string g_insert("INSERT");
static std::string g_remove("REMOVE");
static std::string g_check("CHECK");
static std::string g_info("INFO");

static bool is_command(std::string const & cmd, std::string const & line) {
    return line.compare(0, cmd.size(), cmd) == 0;
}

static std::string & ltrim(std::string & s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    return s;
}

static std::string & rtrim(std::string & s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}

static std::string & trim(std::string & s) {
    return ltrim(rtrim(s));
}

void server::process_from(unsigned linenum) {
    unsigned i = m_file->find(linenum);
    m_file->m_snapshots.resize(i);
    snapshot & s = i == 0 ? m_empty_snapshot : m_file->m_snapshots[i-1];
    std::string block;
    lean_assert(s.m_line > 0);
    m_file->m_info.invalidate(s.m_line-1);
    for (unsigned j = s.m_line-1; j < m_file->m_lines.size(); j++) {
        block += m_file->m_lines[j];
        block += '\n';
    }
    std::istringstream strm(block);
    m_ios.set_options(s.m_options);
    parser p(s.m_env, m_ios, strm, m_file->m_fname.c_str(), false, 1, s.m_lds, s.m_eds, s.m_line,
             &m_file->m_snapshots, &m_file->m_info);
    // p.set_cache(&m_cache);
    p();
}

void server::update() {
    if (m_file->m_from == m_file->m_lines.size())
        return;
    process_from(m_file->m_from);
    m_file->m_from = m_file->m_lines.size();
}

void server::load_file(std::string const & fname) {
    std::ifstream in(fname);
    if (in.bad() || in.fail()) {
        m_out << "-- ERROR failed to open file '" << fname << "'" << std::endl;
    } else {
        m_file.reset(new file(fname));
        m_file_map.insert(mk_pair(fname, m_file));
        for (std::string line; std::getline(in, line);) {
            m_file->m_lines.push_back(line);
        }
    }
}

void server::visit_file(std::string const & fname) {
    auto it = m_file_map.find(fname);
    if (it == m_file_map.end())
        load_file(fname);
    else
        m_file = it->second;
}

void server::show_info(unsigned linenum) {
    check_file();
    update();
    unsigned i = m_file->find(linenum);
    environment const & env = i == 0 ? m_env     : m_file->m_snapshots[i-1].m_env;
    options const & o       = i == 0 ? m_options : m_file->m_snapshots[i-1].m_options;
    m_ios.set_options(o);
    io_state_stream out(env, m_ios);
    out << "-- BEGININFO" << endl;
    m_file->m_info.display(out, linenum);
    out << "-- ENDINFO" << endl;
}

void server::read_line(std::istream & in, std::string & line) {
    if (!std::getline(in, line))
        throw exception("unexpected end of input");
}

// Given a line of the form "cmd linenum", return the linenum
unsigned server::get_linenum(std::string const & line, std::string const & cmd) {
    std::string data = line.substr(cmd.size());
    trim(data);
    unsigned r = atoi(data.c_str());
    if (r == 0)
        throw exception("line numbers are indexed from 1");
    return r;
}

void server::check_file() {
    if (!m_file)
        throw exception("no file has been loaded/visited");
}

void server::replace_line(unsigned linenum, std::string const & new_line) {
    check_file();
    m_file->replace_line(linenum, new_line);
}

void server::insert_line(unsigned linenum, std::string const & new_line) {
    check_file();
    m_file->insert_line(linenum, new_line);
}

void server::remove_line(unsigned linenum) {
    check_file();
    m_file->remove_line(linenum);
}

void server::check_line(unsigned linenum, std::string const & line) {
    check_file();
    if (linenum >= m_file->m_lines.size()) {
        m_out << "-- MISMATCH line out of range" << std::endl;
    } else if (m_file->m_lines[linenum] != line) {
        m_out << "-- MISMATCH expected " << m_file->m_lines[linenum] << std::endl;
    } else {
        m_out << "-- OK" << std::endl;
    }
}

bool server::operator()(std::istream & in) {
    for (std::string line; std::getline(in, line);) {
        try {
            if (is_command(g_load, line)) {
                std::string fname = line.substr(g_load.size());
                trim(fname);
                load_file(fname);
            } else if (is_command(g_visit, line)) {
                std::string fname = line.substr(g_visit.size());
                trim(fname);
                visit_file(fname);
            } else if (is_command(g_check, line)) {
                unsigned linenum = get_linenum(line, g_check);
                read_line(in, line);
                check_line(linenum-1, line);
            } else if (is_command(g_replace, line)) {
                unsigned linenum = get_linenum(line, g_replace);
                read_line(in, line);
                replace_line(linenum-1, line);
            } else if (is_command(g_insert, line)) {
                unsigned linenum = get_linenum(line, g_insert);
                read_line(in, line);
                insert_line(linenum-1, line);
            } else if (is_command(g_remove, line)) {
                unsigned linenum = get_linenum(line, g_remove);
                remove_line(linenum-1);
            } else if (is_command(g_info, line)) {
                unsigned linenum = get_linenum(line, g_info);
                show_info(linenum);
            } else {
                throw exception(sstream() << "unexpected command line: " << line);
            }
        } catch (exception & ex) {
            m_out << "-- ERROR " << ex.what() << std::endl;
        }
    }
    return true;
}
}
