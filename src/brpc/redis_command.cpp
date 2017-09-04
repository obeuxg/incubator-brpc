// Copyright (c) 2015 baidu-rpc authors.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "base/logging.h"
#include "brpc/log.h"
#include "brpc/redis_command.h"

// Defined in public/iobuf/base/iobuf.cpp
void* fast_memcpy(void *__restrict dest, const void *__restrict src, size_t n);


namespace brpc {

// Much faster than snprintf(..., "%lu", d);
inline size_t AppendDecimal(char* outbuf, unsigned long d) {
    char buf[24];  // enough for decimal 64-bit integers
    size_t n = sizeof(buf);
    do {
        const unsigned long q = d / 10;
        buf[--n] = d - q * 10 + '0';
        d = q;
    } while (d);
    fast_memcpy(outbuf, buf + n, sizeof(buf) - n);
    return sizeof(buf) - n;
}

// This function is the hotspot of RedisCommandFormatV() when format is
// short or does not have many %. In a 100K-time call to formating of
// "GET key1", the time spent on RedisRequest.AddCommand() are ~700ns
// vs. ~400ns while using snprintf() vs. AppendDecimal() respectively.
inline void AppendHeader(std::string& buf, char fc, unsigned long value) {
    char header[32];
    header[0] = fc;
    size_t len = AppendDecimal(header + 1, value);
    header[len + 1] = '\r';
    header[len + 2] = '\n';
    buf.append(header, len + 3);
}
inline void AppendHeader(base::IOBuf& buf, char fc, unsigned long value) {
    char header[32];
    header[0] = fc;
    size_t len = AppendDecimal(header + 1, value);
    header[len + 1] = '\r';
    header[len + 2] = '\n';
    buf.append(header, len + 3);
}

// Support hiredis-style format, namely everything is same with printf except
// that %b corresponds to binary-data + length. Notice that we can't use
// %.*s (printf built-in) which ends scaning at \0 and is not binary-safe.
// Some code is copied or modified from redisvFormatCommand() in
// https://github.com/redis/hiredis/blob/master/hiredis.c to keep close
// compatibility with hiredis.
base::Status
RedisCommandFormatV(base::IOBuf* outbuf, const char* fmt, va_list ap) {
    if (outbuf == NULL || fmt == NULL) {
        return base::Status(EINVAL, "Param[outbuf] or [fmt] is NULL");
    }
    const size_t fmt_len = strlen(fmt);
    std::string nocount_buf;
    nocount_buf.reserve(fmt_len * 3 / 2 + 16);
    std::string compbuf;  // A component
    compbuf.reserve(fmt_len + 16);
    const char* c = fmt;
    int ncomponent = 0;
    char quote_char = 0;
    const char* quote_pos = fmt;
    int nargs = 0;
    for (; *c; ++c) {
        if (*c != '%' || c[1] == '\0') {
            if (*c == ' ') {
                if (quote_char) {
                    compbuf.push_back(*c);
                } else if (!compbuf.empty()) {
                    AppendHeader(nocount_buf, '$', compbuf.size());
                    compbuf.append("\r\n", 2);
                    nocount_buf.append(compbuf);
                    compbuf.clear();
                    ++ncomponent;
                }
            } else if (*c == '"' || *c == '\'') {  // Check quotation.
                if (!quote_char) {  // begin quote
                    quote_char = *c;
                    quote_pos = c;
                } else if (quote_char == *c) {  // end quote
                    quote_char = 0;
                } else {
                    compbuf.push_back(*c);
                }
            } else {
                compbuf.push_back(*c);
            }
        } else {
            char *arg;
            size_t size;

            switch(c[1]) {
            case 's':
                arg = va_arg(ap, char*);
                size = strlen(arg);
                if (size > 0) {
                    compbuf.append(arg, size);
                }
                ++nargs;
                break;
            case 'b':
                arg = va_arg(ap, char*);
                size = va_arg(ap, size_t);
                if (size > 0) {
                    compbuf.append(arg, size);
                }
                ++nargs;
                break;
            case '%':
                compbuf.push_back('%');
                break;
            default: {
                /* Try to detect printf format */
                static const char intfmts[] = "diouxX";
                static const char flags[] = "#0-+ ";
                char _format[24];
                char _printed[40];
                const char *_p = c+1;
                size_t _l = 0;
                va_list _cpy;

                /* Flags */
                while (*_p != '\0' && strchr(flags,*_p) != NULL) _p++;

                /* Field width */
                while (*_p != '\0' && isdigit(*_p)) _p++;

                /* Precision */
                if (*_p == '.') {
                    _p++;
                    while (*_p != '\0' && isdigit(*_p)) _p++;
                }

                /* Copy va_list before consuming with va_arg */
                va_copy(_cpy, ap);

                /* Integer conversion (without modifiers) */
                if (strchr(intfmts,*_p) != NULL) {
                    va_arg(ap,int);
                    goto fmt_valid;
                }

                /* Double conversion (without modifiers) */
                if (strchr("eEfFgGaA",*_p) != NULL) {
                    va_arg(ap,double);
                    goto fmt_valid;
                }

                /* Size: char */
                if (_p[0] == 'h' && _p[1] == 'h') {
                    _p += 2;
                    if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                        va_arg(ap,int); /* char gets promoted to int */
                        goto fmt_valid;
                    }
                    goto fmt_invalid;
                }

                /* Size: short */
                if (_p[0] == 'h') {
                    _p += 1;
                    if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                        va_arg(ap,int); /* short gets promoted to int */
                        goto fmt_valid;
                    }
                    goto fmt_invalid;
                }

                /* Size: long long */
                if (_p[0] == 'l' && _p[1] == 'l') {
                    _p += 2;
                    if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                        va_arg(ap,long long);
                        goto fmt_valid;
                    }
                    goto fmt_invalid;
                }

                /* Size: long */
                if (_p[0] == 'l') {
                    _p += 1;
                    if (*_p != '\0' && strchr(intfmts,*_p) != NULL) {
                        va_arg(ap,long);
                        goto fmt_valid;
                    }
                    goto fmt_invalid;
                }
                
            fmt_invalid:
                va_end(_cpy);
                return base::Status(EINVAL, "Invalid format");

            fmt_valid:
                ++nargs;
                _l = _p + 1 - c;
                if (_l < sizeof(_format)-2) {
                    memcpy(_format, c, _l);
                    _format[_l] = '\0';
                    int plen = vsnprintf(_printed, sizeof(_printed), _format, _cpy);
                    if (plen > 0) {
                        compbuf.append(_printed, plen);
                    }
                    /* Update current position (note: outer blocks
                     * increment c twice so compensate here) */
                    c = _p - 1;
                }
                va_end(_cpy);
                break;
            }  // end default
            }  // end switch
            
            ++c;
        }
    }
    if (quote_char) {
        return base::Status(EINVAL, "Unmatched quote: ... %.*s ... (offset=%lu)",
                            (int)(fmt + fmt_len - quote_pos),
                            quote_pos, quote_pos - fmt);
    }
    
    if (!compbuf.empty()) {
        AppendHeader(nocount_buf, '$', compbuf.size());
        compbuf.append("\r\n", 2);
        nocount_buf.append(compbuf);
        compbuf.clear();
        ++ncomponent;
    }

    LOG_IF(ERROR, nargs == 0) << "You must call RedisCommandNoFormat() "
        "to replace RedisCommandFormatV without any args (to avoid potential "
        "formatting of conversion specifiers)";
    
    AppendHeader(*outbuf, '*', ncomponent);
    outbuf->append(nocount_buf);
    return base::Status::OK();
}

base::Status RedisCommandFormat(base::IOBuf* buf, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const base::Status st = RedisCommandFormatV(buf, fmt, ap);
    va_end(ap);
    return st;
}

base::Status
RedisCommandNoFormat(base::IOBuf* outbuf, const base::StringPiece& cmd) {
    if (outbuf == NULL || cmd == NULL) {
        return base::Status(EINVAL, "Param[outbuf] or [cmd] is NULL");
    }
    const size_t cmd_len = cmd.size();
    std::string nocount_buf;
    nocount_buf.reserve(cmd_len * 3 / 2 + 16);
    std::string compbuf;  // A component
    compbuf.reserve(cmd_len + 16);
    int ncomponent = 0;
    char quote_char = 0;
    const char* quote_pos = cmd.data();
    for (const char* c = cmd.data(); c != cmd.data() + cmd.size(); ++c) {
        if (*c == ' ') {
            if (quote_char) {
                compbuf.push_back(*c);
            } else if (!compbuf.empty()) {
                AppendHeader(nocount_buf, '$', compbuf.size());
                compbuf.append("\r\n", 2);
                nocount_buf.append(compbuf);
                compbuf.clear();
                ++ncomponent;
            }
        } else if (*c == '"' || *c == '\'') {  // Check quotation.
            if (!quote_char) {  // begin quote
                quote_char = *c;
                quote_pos = c;
            } else if (quote_char == *c) {  // end quote
                quote_char = 0;
            } else {
                compbuf.push_back(*c);
            }
        } else {
            compbuf.push_back(*c);
        }
    }
    if (quote_char) {
        return base::Status(EINVAL, "Unmatched quote: ... %.*s ... (offset=%lu)",
                            (int)(cmd.data() + cmd.size() - quote_pos),
                            quote_pos, quote_pos - cmd.data());
    }
    
    if (!compbuf.empty()) {
        AppendHeader(nocount_buf, '$', compbuf.size());
        compbuf.append("\r\n", 2);
        nocount_buf.append(compbuf);
        compbuf.clear();
        ++ncomponent;
    }

    AppendHeader(*outbuf, '*', ncomponent);
    outbuf->append(nocount_buf);
    return base::Status::OK();
}

base::Status RedisCommandByComponents(base::IOBuf* output,
                                      const base::StringPiece* components,
                                      size_t ncomponents) {
    if (output == NULL) {
        return base::Status(EINVAL, "Param[output] is NULL");
    }
    AppendHeader(*output, '*', ncomponents);
    for (size_t i = 0; i < ncomponents; ++i) {
        AppendHeader(*output, '$', components[i].size());
        output->append(components[i].data(), components[i].size());
        output->append("\r\n", 2);
    }
    return base::Status::OK();
}

} // namespace brpc