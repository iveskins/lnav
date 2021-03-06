/**
 * Copyright (c) 2015, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file yajlpp.cc
 */

#include "config.h"

#include "fmt/format.h"

#include "yajlpp.hh"
#include "yajlpp_def.hh"
#include "yajl/api/yajl_parse.h"

using namespace std;

const json_path_handler_base::enum_value_t json_path_handler_base::ENUM_TERMINATOR((const char *) nullptr, 0);

yajl_gen_status json_path_handler_base::gen(yajlpp_gen_context &ygc, yajl_gen handle) const
{
    vector<string> local_paths;

    if (this->jph_path_provider) {
        this->jph_path_provider(ygc.ygc_obj_stack.top(), local_paths);
    }
    else {
        local_paths.emplace_back(this->jph_path);
    }

    if (this->jph_children) {
        for (const auto &lpath : local_paths) {
            string full_path = lpath;
            if (this->jph_path_provider) {
                full_path += "/";
            }
            int start = lpath[0] == '^' ? 1 : 0;
            int start_depth = ygc.ygc_depth;

            for (int lpc = start; lpath[lpc]; lpc++) {
                if (lpath[lpc] == '/') {
                    if (lpc > start) {
                        yajl_gen_pstring(handle,
                                         &lpath[start],
                                         lpc - start);
                        yajl_gen_map_open(handle);
                        ygc.ygc_depth += 1;
                    }
                    start = lpc + 1;
                }
            }
            if (this->jph_path_provider) {
                yajl_gen_pstring(handle, &lpath[start], -1);
                yajl_gen_map_open(handle);
                ygc.ygc_depth += 1;
            }

            if (this->jph_obj_provider) {
                pcre_context_static<30> pc;
                pcre_input pi(full_path);

                this->jph_regex.match(pc, pi);
                ygc.ygc_obj_stack.push(this->jph_obj_provider(
                    {{pc, pi}, -1}, ygc.ygc_obj_stack.top()
                ));
                if (!ygc.ygc_default_stack.empty()) {
                    ygc.ygc_default_stack.push(this->jph_obj_provider(
                        {{pc, pi}, -1}, ygc.ygc_default_stack.top()
                    ));
                }
            }

            for (int lpc = 0; this->jph_children[lpc].jph_path[0]; lpc++) {
                json_path_handler_base &jph = this->jph_children[lpc];
                yajl_gen_status status = jph.gen(ygc, handle);

                if (status != yajl_gen_status_ok) {
                    return status;
                }
            }

            if (this->jph_obj_provider) {
                ygc.ygc_obj_stack.pop();
                if (!ygc.ygc_default_stack.empty()) {
                    ygc.ygc_default_stack.pop();
                }
            }

            while (ygc.ygc_depth > start_depth) {
                yajl_gen_map_close(handle);
                ygc.ygc_depth -= 1;
            }
        }
    }
    else if (this->jph_gen_callback != NULL) {
        return this->jph_gen_callback(ygc, *this, handle);
    }

    return yajl_gen_status_ok;
}

void json_path_handler_base::walk(
    const std::function<void(const json_path_handler_base &,
                             const std::string &,
                             void *)> &cb,
    void *root, const string &base) const
{
    vector<string> local_paths;

    if (this->jph_path_provider) {
        this->jph_path_provider(root, local_paths);

        for (auto &lpath : local_paths) {
            cb(*this, lpath, nullptr);
        }
    }
    else {
        local_paths.emplace_back(this->jph_path);
    }

    if (this->jph_children) {
        for (const auto &lpath : local_paths) {
            for (int lpc = 0; this->jph_children[lpc].jph_path[0]; lpc++) {
                string full_path = base + lpath;
                if (this->jph_path_provider) {
                    full_path += "/";
                }
                json_path_handler dummy[] = {
                    json_path_handler(this->jph_path),

                    json_path_handler()
                };
                dummy->jph_callbacks = this->jph_callbacks;

                yajlpp_parse_context ypc("possibilities", dummy);
                void *child_root = root;

                ypc.set_path(full_path)
                    .with_obj(root)
                    .update_callbacks();
                if (this->jph_obj_provider) {
                    string full_path = lpath + "/";
                    pcre_input pi(full_path);

                    if (!this->jph_regex.match(ypc.ypc_pcre_context, pi)) {
                        ensure(false);
                    }
                    child_root = this->jph_obj_provider(
                        {{ypc.ypc_pcre_context, pi}, -1}, root);
                }

                this->jph_children[lpc].walk(cb, child_root, full_path);
            }
        }
    }
    else {
        for (auto &lpath : local_paths) {
            void *field = nullptr;

            if (this->jph_field_getter) {
                field = this->jph_field_getter(root, lpath);
            }
            cb(*this, base + lpath, field);
        }
    }
}

int yajlpp_parse_context::map_start(void *ctx)
{
    yajlpp_parse_context *ypc = (yajlpp_parse_context *)ctx;
    int retval = 1;

    ypc->ypc_path_index_stack.push_back(ypc->ypc_path.size() - 1);

    if (ypc->ypc_path.size() > 1 &&
        ypc->ypc_path[ypc->ypc_path.size() - 2] == '#') {
        ypc->ypc_array_index.back() += 1;
    }

    if (ypc->ypc_alt_callbacks.yajl_start_map != NULL) {
        retval = ypc->ypc_alt_callbacks.yajl_start_map(ypc);
    }

    return retval;
}

int yajlpp_parse_context::map_key(void *ctx,
                                  const unsigned char *key,
                                  size_t len)
{
    yajlpp_parse_context *ypc = (yajlpp_parse_context *)ctx;
    int retval = 1;

    ypc->ypc_path.resize(ypc->ypc_path_index_stack.back());
    ypc->ypc_path.push_back('/');
    if (ypc->ypc_handlers != NULL) {
        for (size_t lpc = 0; lpc < len; lpc++) {
            switch (key[lpc]) {
                case '~':
                    ypc->ypc_path.push_back('~');
                    ypc->ypc_path.push_back('0');
                    break;
                case '/':
                    ypc->ypc_path.push_back('~');
                    ypc->ypc_path.push_back('1');
                    break;
                case '#':
                    ypc->ypc_path.push_back('~');
                    ypc->ypc_path.push_back('2');
                    break;
                default:
                    ypc->ypc_path.push_back(key[lpc]);
                    break;
            }
        }
    }
    else {
        size_t start = ypc->ypc_path.size();
        ypc->ypc_path.resize(ypc->ypc_path.size() + len);
        memcpy(&ypc->ypc_path[start], key, len);
    }
    ypc->ypc_path.push_back('\0');

    if (ypc->ypc_alt_callbacks.yajl_map_key != NULL) {
        retval = ypc->ypc_alt_callbacks.yajl_map_key(ctx, key, len);
    }

    if (ypc->ypc_handlers != NULL) {
        ypc->update_callbacks();
    }
    return retval;
}

void yajlpp_parse_context::update_callbacks(const json_path_handler_base *orig_handlers, int child_start)
{
    const json_path_handler_base *handlers = orig_handlers;

    this->ypc_current_handler = NULL;

    if (this->ypc_handlers == NULL) {
        return;
    }

    this->ypc_sibling_handlers = orig_handlers;

    pcre_input pi(&this->ypc_path[0], 0, this->ypc_path.size() - 1);

    this->ypc_callbacks = DEFAULT_CALLBACKS;

    if (handlers == NULL) {
        handlers = this->ypc_handlers;
        this->ypc_handler_stack.clear();
    }

    if (!this->ypc_active_paths.empty()) {
        string curr_path(&this->ypc_path[0], this->ypc_path.size() - 1);

        if (this->ypc_active_paths.find(curr_path) ==
            this->ypc_active_paths.end()) {
            return;
        }
    }

    if (child_start == 0 && !this->ypc_obj_stack.empty()) {
        while (this->ypc_obj_stack.size() > 1) {
            this->ypc_obj_stack.pop();
        }
    }

    for (int lpc = 0; handlers[lpc].jph_path[0]; lpc++) {
        const json_path_handler_base &jph = handlers[lpc];

        pi.reset(&this->ypc_path[child_start],
                 0,
                 this->ypc_path.size() - 1 - child_start);
        if (jph.jph_regex.match(this->ypc_pcre_context, pi)) {
            pcre_context::capture_t *cap = this->ypc_pcre_context.all();

            if (jph.jph_obj_provider) {
                this->ypc_obj_stack.push(jph.jph_obj_provider(
                    {{this->ypc_pcre_context, pi}, this->index_for_provider()},
                    this->ypc_obj_stack.top()));
            }

            if (jph.jph_children) {
                char last = this->ypc_path[child_start + cap->c_end - 1];

                if (last != '/') {
                    continue;
                }

                this->ypc_handler_stack.emplace_back(&jph);

                if (child_start + cap->c_end != (int)this->ypc_path.size() - 1) {
                    this->update_callbacks(jph.jph_children,
                                           child_start + cap->c_end);
                }
            }
            else {
                if (child_start + cap->c_end != (int)this->ypc_path.size() - 1) {
                    continue;
                }

                this->ypc_current_handler = &jph;
            }

            if (jph.jph_callbacks.yajl_null != nullptr)
                this->ypc_callbacks.yajl_null = jph.jph_callbacks.yajl_null;
            if (jph.jph_callbacks.yajl_boolean != nullptr)
                this->ypc_callbacks.yajl_boolean = jph.jph_callbacks.yajl_boolean;
            if (jph.jph_callbacks.yajl_integer != nullptr)
                this->ypc_callbacks.yajl_integer = jph.jph_callbacks.yajl_integer;
            if (jph.jph_callbacks.yajl_double != nullptr)
                this->ypc_callbacks.yajl_double = jph.jph_callbacks.yajl_double;
            if (jph.jph_callbacks.yajl_string != nullptr)
                this->ypc_callbacks.yajl_string = jph.jph_callbacks.yajl_string;

            return;
        }
    }

    this->ypc_handler_stack.emplace_back(nullptr);
}

int yajlpp_parse_context::map_end(void *ctx)
{
    yajlpp_parse_context *ypc = (yajlpp_parse_context *)ctx;
    int retval = 1;

    ypc->ypc_path.resize(ypc->ypc_path_index_stack.back());
    ypc->ypc_path.push_back('\0');
    ypc->ypc_path_index_stack.pop_back();

    if (ypc->ypc_alt_callbacks.yajl_end_map != NULL) {
        retval = ypc->ypc_alt_callbacks.yajl_end_map(ctx);
    }

    ypc->update_callbacks();
    return retval;
}

int yajlpp_parse_context::array_start(void *ctx)
{
    yajlpp_parse_context *ypc = (yajlpp_parse_context *)ctx;
    int retval = 1;

    ypc->ypc_path_index_stack.push_back(ypc->ypc_path.size() - 1);
    ypc->ypc_path[ypc->ypc_path.size() - 1] = '#';
    ypc->ypc_path.push_back('\0');
    ypc->ypc_array_index.push_back(-1);

    if (ypc->ypc_alt_callbacks.yajl_start_array != NULL) {
        retval = ypc->ypc_alt_callbacks.yajl_start_array(ctx);
    }

    ypc->update_callbacks();

    return retval;
}

int yajlpp_parse_context::array_end(void *ctx)
{
    yajlpp_parse_context *ypc = (yajlpp_parse_context *)ctx;
    int retval = 1;

    ypc->ypc_path.resize(ypc->ypc_path_index_stack.back());
    ypc->ypc_path.push_back('\0');
    ypc->ypc_path_index_stack.pop_back();
    ypc->ypc_array_index.pop_back();

    if (ypc->ypc_alt_callbacks.yajl_end_array != NULL) {
        retval = ypc->ypc_alt_callbacks.yajl_end_array(ctx);
    }

    ypc->update_callbacks();

    return retval;
}

int yajlpp_parse_context::handle_unused(void *ctx)
{
    yajlpp_parse_context *ypc = (yajlpp_parse_context *)ctx;

    if (ypc->ypc_ignore_unused) {
        return 1;
    }

    const json_path_handler_base *handler = ypc->ypc_current_handler;

    int line_number = ypc->get_line_number();

    if (handler != nullptr && strlen(handler->jph_synopsis) > 0 &&
        strlen(handler->jph_description) > 0) {

        ypc->report_error(
            lnav_log_level_t::WARNING,
            "%s:line %d",
            ypc->ypc_source.c_str(),
            line_number);
        ypc->report_error(lnav_log_level_t::WARNING, "  unexpected data for path");

        ypc->report_error(lnav_log_level_t::WARNING,
                          "    %s %s -- %s",
                          &ypc->ypc_path[0],
                          handler->jph_synopsis,
                          handler->jph_description);
    }
    else if (ypc->ypc_path[0]) {
        ypc->report_error(lnav_log_level_t::WARNING,
                          "%s:line %d",
                          ypc->ypc_source.c_str(),
                          line_number);
        ypc->report_error(lnav_log_level_t::WARNING, "  unexpected path --");

        ypc->report_error(lnav_log_level_t::WARNING, "    %s", &ypc->ypc_path[0]);
    } else {
        ypc->report_error(lnav_log_level_t::WARNING,
                          "%s:line %d\n  unexpected JSON value",
                          ypc->ypc_source.c_str(),
                          line_number);
    }

    if (ypc->ypc_callbacks.yajl_boolean != (int (*)(void *, int))yajlpp_parse_context::handle_unused ||
        ypc->ypc_callbacks.yajl_integer != (int (*)(void *, long long))yajlpp_parse_context::handle_unused ||
        ypc->ypc_callbacks.yajl_double != (int (*)(void *, double))yajlpp_parse_context::handle_unused ||
        ypc->ypc_callbacks.yajl_string != (int (*)(void *, const unsigned char *, size_t))yajlpp_parse_context::handle_unused) {
        ypc->report_error(lnav_log_level_t::WARNING, "  expecting one of the following data types --");
    }

    if (ypc->ypc_callbacks.yajl_boolean != (int (*)(void *, int))yajlpp_parse_context::handle_unused) {
        ypc->report_error(lnav_log_level_t::WARNING, "    boolean");
    }
    if (ypc->ypc_callbacks.yajl_integer != (int (*)(void *, long long))yajlpp_parse_context::handle_unused) {
        ypc->report_error(lnav_log_level_t::WARNING, "    integer");
    }
    if (ypc->ypc_callbacks.yajl_double != (int (*)(void *, double))yajlpp_parse_context::handle_unused) {
        ypc->report_error(lnav_log_level_t::WARNING, "    float");
    }
    if (ypc->ypc_callbacks.yajl_string != (int (*)(void *, const unsigned char *, size_t))yajlpp_parse_context::handle_unused) {
        ypc->report_error(lnav_log_level_t::WARNING, "    string");
    }

    if (handler == nullptr) {
        const json_path_handler_base *accepted_handlers;

        if (ypc->ypc_sibling_handlers) {
            accepted_handlers = ypc->ypc_sibling_handlers;
        } else {
            accepted_handlers = ypc->ypc_handlers;
        }

        ypc->report_error(lnav_log_level_t::WARNING, "  accepted paths --");
        for (int lpc = 0; accepted_handlers[lpc].jph_path[0]; lpc++) {
            ypc->report_error(lnav_log_level_t::WARNING, "    %s %s -- %s",
                    accepted_handlers[lpc].jph_path,
                    accepted_handlers[lpc].jph_synopsis,
                    accepted_handlers[lpc].jph_description);
        }
    }

    return 1;
}

const yajl_callbacks yajlpp_parse_context::DEFAULT_CALLBACKS = {
    yajlpp_parse_context::handle_unused,
    (int (*)(void *, int))yajlpp_parse_context::handle_unused,
    (int (*)(void *, long long))yajlpp_parse_context::handle_unused,
    (int (*)(void *, double))yajlpp_parse_context::handle_unused,
    NULL,
    (int (*)(void *, const unsigned char *, size_t))
    yajlpp_parse_context::handle_unused,
    yajlpp_parse_context::map_start,
    yajlpp_parse_context::map_key,
    yajlpp_parse_context::map_end,
    yajlpp_parse_context::array_start,
    yajlpp_parse_context::array_end,
};

yajl_status
yajlpp_parse_context::parse(const unsigned char *jsonText, size_t jsonTextLen)
{
    this->ypc_json_text = jsonText;

    yajl_status retval = yajl_parse(this->ypc_handle, jsonText, jsonTextLen);

    size_t consumed = yajl_get_bytes_consumed(this->ypc_handle);

    this->ypc_line_number += std::count(&jsonText[0], &jsonText[consumed], '\n');

    this->ypc_json_text = NULL;

    if (retval != yajl_status_ok && this->ypc_error_reporter) {
        this->ypc_error_reporter(
            *this, lnav_log_level_t::ERROR,
            fmt::format("error:{}:{}:invalid json -- {}",
                        this->ypc_source,
                        this->get_line_number(),
                        yajl_get_error(this->ypc_handle, 1,
                                       jsonText, jsonTextLen)).c_str());
    }

    return retval;
}

yajl_status yajlpp_parse_context::complete_parse()
{
    yajl_status retval = yajl_complete_parse(this->ypc_handle);

    if (retval != yajl_status_ok && this->ypc_error_reporter) {
        this->ypc_error_reporter(
            *this, lnav_log_level_t::ERROR,
            fmt::format("error:{}:invalid json -- {}",
                        this->ypc_source,
                        yajl_get_error(this->ypc_handle, 0,
                                       nullptr, 0)).c_str());
    }

    return retval;
}

void yajlpp_gen_context::gen()
{
    yajlpp_map root(this->ygc_handle);

    for (int lpc = 0; this->ygc_handlers[lpc].jph_path[0]; lpc++) {
        json_path_handler &jph = this->ygc_handlers[lpc];

        jph.gen(*this, this->ygc_handle);
    }
}

yajlpp_gen_context &yajlpp_gen_context::with_context(yajlpp_parse_context &ypc)
{
    this->ygc_obj_stack = ypc.ypc_obj_stack;
    if (ypc.ypc_current_handler == nullptr &&
        !ypc.ypc_handler_stack.empty() &&
        ypc.ypc_handler_stack.back() != nullptr) {
        this->ygc_handlers = static_cast<json_path_handler *>(ypc.ypc_handler_stack.back()->jph_children);
        this->ygc_depth += 1;
    }
    return *this;
}
