/*
Copyright (c) 2003, 2004 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "polipo.h"

AtomPtr proxyName = NULL;
int proxyPort = 8123;

AtomPtr authRealm = NULL;
AtomPtr authCredentials = NULL;

AtomListPtr allowedClients = NULL;
NetAddressPtr allowedNets = NULL;

IntListPtr allowedPorts = NULL;
IntListPtr tunnelAllowedPorts = NULL;
int expectContinue = 1;

AtomPtr atom100Continue;

/* 0 means that all failures lead to errors.  1 means that failures to
   connect are reported in a Warning header when stale objects are
   served.  2 means that only missing data is fetched from the net,
   stale data is served without revalidation (browser-side
   Cache-Control directives are still honoured).  3 means that no
   connections are ever attempted. */

int proxyOffline = 0;
int relaxTransparency = 0;
AtomPtr proxyAddress = NULL;

void
preinitHttp()
{
    proxyAddress = internAtom("127.0.0.1");
    CONFIG_VARIABLE(proxyOffline, CONFIG_BOOLEAN,
                    "Avoid contacting remote servers.");
    CONFIG_VARIABLE(relaxTransparency, CONFIG_TRISTATE,
                    "Avoid contacting remote servers.");
    CONFIG_VARIABLE(proxyPort, CONFIG_INT,
                    "The TCP port on which the proxy listens.");
    CONFIG_VARIABLE(proxyAddress, CONFIG_ATOM_LOWER,
                    "The IP address on which the proxy listens.");
    CONFIG_VARIABLE(proxyName, CONFIG_ATOM_LOWER,
                    "The name under which the proxy is known.");
    CONFIG_VARIABLE(authRealm, CONFIG_ATOM,
                    "Authentication realm.");
    CONFIG_VARIABLE(authCredentials, CONFIG_ATOM,
                    "username:password.");
    CONFIG_VARIABLE(allowedClients, CONFIG_ATOM_LIST_LOWER,
                    "Networks from which clients are allowed to connect.");
    CONFIG_VARIABLE(tunnelAllowedPorts, CONFIG_INT_LIST,
                    "Ports to which tunnelled connections are allowed.");
    CONFIG_VARIABLE(allowedPorts, CONFIG_INT_LIST,
                    "Ports to which connections are allowed.");
    CONFIG_VARIABLE(expectContinue, CONFIG_TRISTATE,
                    "Send Expect-Continue to servers.");
    preinitHttpParser();
}

void
initHttp()
{
    char *buf = get_chunk();
    int namelen;
    int n;
    struct hostent *host;

    initHttpParser();

    atom100Continue = internAtom("100-continue");

    if(authCredentials != NULL && authRealm == NULL)
        authRealm = internAtom("Polipo");

    if(allowedClients) {
        allowedNets = parseNetAddress(allowedClients);
        if(allowedNets == NULL)
            exit(1);
    }

    if(allowedPorts == NULL) {
        allowedPorts = makeIntList(0);
        if(allowedPorts == NULL) {
            do_log(L_ERROR, "Couldn't allocate allowedPorts.\n");
            exit(1);
        }
        intListCons(80, 86, allowedPorts);
        intListCons(1024, 0xFFFF, allowedPorts);
    }

    if(tunnelAllowedPorts == NULL) {
        tunnelAllowedPorts = makeIntList(0);
        if(tunnelAllowedPorts == NULL) {
            do_log(L_ERROR, "Couldn't allocate tunnelAllowedPorts.\n");
            exit(1);
        }
        intListCons(22, 22, tunnelAllowedPorts);
        intListCons(80, 80, tunnelAllowedPorts);
        intListCons(443, 443, tunnelAllowedPorts);
    }

    if(proxyName)
        return;

    if(buf == NULL) {
        do_log(L_ERROR, "Couldn't allocate chunk for host name.\n");
        goto fail;
    }

    n = gethostname(buf, CHUNK_SIZE);
    if(n != 0) {
        do_log_error(L_WARN, errno, "Gethostname");
        strcpy(buf, "polipo");
        goto success;
    }
    /* gethostname doesn't necessarily NUL-terminate on overflow */
    buf[CHUNK_SIZE - 1] = '\0';

    if(strcmp(buf, "(none)") == 0) {
        do_log(L_WARN, "Couldn't determine host name -- using ``polipo''.\n");
        strcpy(buf, "polipo");
        goto success;
    }

    if(strchr(buf, '.') != NULL)
        goto success;

    host = gethostbyname(buf);
    if(host == NULL) {
        goto success;
    }

    if(host->h_addrtype != AF_INET)
        goto success;

    host = gethostbyaddr(host->h_addr_list[0], host->h_length,  AF_INET);

    if(!host || !host->h_name || strcmp(host->h_name, "localhost") == 0)
        goto success;

    namelen = strlen(host->h_name);
    if(namelen >= CHUNK_SIZE) {
        do_log(L_ERROR, "Host name too long.\n");
        goto success;
    }

    memcpy(buf, host->h_name, namelen + 1);

 success:
    proxyName = internAtom(buf);
    if(proxyName == NULL) {
        do_log(L_ERROR, "Couldn't allocate proxy name.\n");
        goto fail;
    }
    dispose_chunk(buf);
    return;

 fail:
    if(buf)
        dispose_chunk(buf);
    exit(1);
    return;
}

int
httpSetTimeout(HTTPConnectionPtr connection, int secs)
{
    TimeEventHandlerPtr new;

    if(connection->timeout)
        cancelTimeEvent(connection->timeout);
    connection->timeout = NULL;

    if(secs > 0) {
        new = scheduleTimeEvent(secs, httpTimeoutHandler,
                                sizeof(connection), &connection);
        if(!new) {
            do_log(L_ERROR, "Couldn't schedule timeout for connection 0x%x\n",
                   (unsigned)connection);
            return -1;
        }
    } else {
        new = NULL;
    }

    connection->timeout = new;
    return 1;
}

int 
httpTimeoutHandler(TimeEventHandlerPtr event)
{
    HTTPConnectionPtr connection = *(HTTPConnectionPtr*)event->data;

    if(connection->fd >= 0) {
        int rc;
        rc = shutdown(connection->fd, 2);
        if(rc < 0 && errno != ENOTCONN)
                do_log_error(L_ERROR, errno, "Timeout: shutdown failed");
        pokeFdEvent(connection->fd, -EDOTIMEOUT, POLLIN | POLLOUT);
    }
    connection->timeout = NULL;
    return 1;
}

int
httpWriteObjectHeaders(char *buf, int offset, int len,
                       ObjectPtr object, int from, int to)
{
    int n = offset;

    if(from <= 0 && to < 0) {
        if(object->length >= 0) {
            n = snnprintf(buf, n, len,
                          "\r\nContent-Length: %d", object->length);
        }
    } else {
        if(to >= 0) {
            n = snnprintf(buf, n, len,
                          "\r\nContent-Length: %d", to - from);
        }
    }

    if(from > 0 || to > 0) {
        if(object->length >= 0) {
            if(from >= to) {
                n = snnprintf(buf, n, len,
                              "\r\nContent-Range: bytes */%d",
                              object->length);
            } else {
                n = snnprintf(buf, n, len,
                              "\r\nContent-Range: bytes %d-%d/%d",
                              from, to - 1, 
                              object->length);
            }
        } else {
            if(to >= 0) {
                n = snnprintf(buf, n, len,
                              "\r\nContent-Range: bytes %d-/*",
                              from);
            } else {
                n = snnprintf(buf, n, len,
                              "\r\nContent-Range: bytes %d-%d/*",
                              from, to);
            }
        }
    }
        
    if(object->etag) {
        n = snnprintf(buf, n, len, "\r\nETag: \"%s\"", object->etag);
    }
    if((object->flags & OBJECT_LOCAL) || object->date >= 0) {
        n = snnprintf(buf, n, len, "\r\nDate: ");
        n = format_time(buf, n, len, 
                        (object->flags & OBJECT_LOCAL) ?
                        current_time.tv_sec : object->date);
        if(n < 0)
            goto fail;
    }

    if(object->last_modified >= 0) {
        n = snnprintf(buf, n, len, "\r\nLast-Modified: ");
        n = format_time(buf, n, len, object->last_modified);
        if(n < 0)
            goto fail;
    }

    if(object->expires >= 0) {
        n = snnprintf(buf, n, len, "\r\nExpires: ");
        n = format_time(buf, n, len, object->expires);
        if(n < 0)
            goto fail;
    }

    n = httpPrintCacheControl(buf, n, len,
                              object->cache_control, NULL);
    if(n < 0)
        goto fail;

    if(object->via)
        n = snnprintf(buf, n, len, "\r\nVia: %s", object->via->string);

    if(object->headers)
        n = snnprint_n(buf, n, len, object->headers->string,
                       object->headers->length);

    if(n < len)
        return n;
    else
        return -1;

 fail:
    return -1;
}

static int
cachePrintSeparator(char *buf, int offset, int len,
                    int subsequent)
{
    int n = offset;
    if(subsequent)
        n = snnprintf(buf, offset, len, ", ");
    else
        n = snnprintf(buf, offset, len, "\r\nCache-Control: ");
    return n;
}

int
httpPrintCacheControl(char *buf, int offset, int len,
                      int flags, CacheControlPtr cache_control)
{
    int n = offset;
    int sub = 0;

#define PRINT_SEP() \
    do {\
        n = cachePrintSeparator(buf, n, len, sub); \
        sub = 1; \
    } while(0)

    if(cache_control)
        flags |= cache_control->flags;

    if(flags & CACHE_NO) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "no-cache");
    }
    if(flags & CACHE_PUBLIC) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "public");
    }
    if(flags & CACHE_PRIVATE) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "private");
    }
    if(flags & CACHE_NO_STORE) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "no-store");
    }
    if(flags & CACHE_NO_TRANSFORM) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "no-transform");
    }
    if(flags & CACHE_MUST_REVALIDATE) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "must-revalidate");
    }
    if(flags & CACHE_PROXY_REVALIDATE) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "proxy-revalidate");
    }
    if(flags & CACHE_ONLY_IF_CACHED) {
        PRINT_SEP();
        n = snnprintf(buf, n, len, "only-if-cached");
    }
    if(cache_control) {
        if(cache_control->max_age >= 0) {
            PRINT_SEP();
            n = snnprintf(buf, n, len, "max-age=%d",
                          cache_control->max_age);
        }
        if(cache_control->s_maxage >= 0) {
            PRINT_SEP();
            n = snnprintf(buf, n, len, "s-maxage=%d", 
                          cache_control->s_maxage);
        }
        if(cache_control->min_fresh > 0) {
            PRINT_SEP();
            n = snnprintf(buf, n, len, "min-fresh=%d",
                          cache_control->min_fresh);
        }
        if(cache_control->max_stale > 0) {
            PRINT_SEP();
            n = snnprintf(buf, n, len, "max-stale=%d",
                          cache_control->min_fresh);
        }
    }
    if(flags & CACHE_NO) {
        n = snnprintf(buf, n, len, "\r\nPragma: no-cache");
    }
    return n;
#undef PRINT_SEP
}

char *
httpMessage(int code)
{
    switch(code) {
    case 200:
        return "Okay";
    case 206:
        return "Partial content";
    case 300:
        return "Multiple choices";
    case 301:
        return "Moved permanently";
    case 302:
        return "Found";
    case 303:
        return "See other";
    case 304:
        return "Not changed";
    case 307:
        return "Temporary redirect";
    case 401:
        return "Authentication Required";
    case 403:
        return "Forbidden";
    case 404:
        return "Not found";
    case 405:
        return "Method not allowed";
    case 407:
        return "Proxy authentication required";
    default:
        return "Unknown error code";
    }
}

int
htmlString(char *buf, int n, int len, char *s, int slen)
{
    int i = 0;
    while(i < slen && n + 5 < len) {
        switch(s[i]) {
        case '&':
            buf[n++] = '&'; buf[n++] = 'a'; buf[n++] = 'm'; buf[n++] = 'p';
            buf[n++] = ';';
            break;
        case '<':
            buf[n++] = '&'; buf[n++] = 'l'; buf[n++] = 't'; buf[n++] = ';';
            break;
        case '>':
            buf[n++] = '&'; buf[n++] = 'g'; buf[n++] = 't'; buf[n++] = ';';
            break;
        case '"':
            buf[n++] = '&'; buf[n++] = 'q'; buf[n++] = 'u'; buf[n++] = 'o';
            buf[n++] = 't'; buf[n++] = ';';
            break;
        case '\0':
            break;
        default:
            buf[n++] = s[i];
        }
        i++;
    }
    return n;
}

void
htmlPrint(FILE *out, char *s, int slen)
{
    int i;
    for(i = 0; i < slen; i++) {
        switch(s[i]) {
        case '&':
            fputs("&amp;", out);
            break;
        case '<':
            fputs("&lt;", out);
            break;
        case '>':
            fputs("&gt;", out);
            break;
        default:
            fputc(s[i], out);
        }
    }
}

HTTPConnectionPtr
httpMakeConnection()
{
    HTTPConnectionPtr connection;
    connection = malloc(sizeof(HTTPConnectionRec));
    if(connection == NULL)
        return NULL;
    connection->flags = 0;
    connection->fd = -1;
    connection->buf = NULL;
    connection->len = 0;
    connection->offset = 0;
    connection->request = NULL;
    connection->request_last = NULL;
    connection->serviced = 0;
    connection->version = HTTP_UNKNOWN;
    connection->timeout = NULL;
    connection->te = TE_IDENTITY;
    connection->reqbuf = NULL;
    connection->reqlen = 0;
    connection->reqbegin = 0;
    connection->reqoffset = 0;
    connection->bodylen = -1;
    connection->reqte = TE_IDENTITY;
    connection->chunk_remaining = 0;
    connection->server = NULL;
    connection->pipelined = 0;
    connection->connecting = 0;
    connection->server = NULL;
    return connection;
}

void
httpDestroyConnection(HTTPConnectionPtr connection)
{
    assert(connection->flags == 0);
    if(connection->buf)
        dispose_chunk(connection->buf);
    assert(!connection->request);
    assert(!connection->request_last);
    dispose_chunk(connection->reqbuf);
    assert(!connection->timeout);
    assert(!connection->server);
    free(connection);
}

HTTPRequestPtr 
httpMakeRequest()

{
    HTTPRequestPtr request;
    request = malloc(sizeof(HTTPRequestRec));
    if(request == NULL)
        return NULL;
    request->connection = NULL;
    request->object = NULL;
    request->method = METHOD_UNKNOWN;
    request->from = 0;
    request->to = -1;
    request->cache_control = no_cache_control;
    request->condition = NULL;
    request->via = NULL;
    request->persistent = 0;
    request->wait_continue = 0;
    request->ohandler = NULL;
    request->requested = 0;
    request->force_error = 0;
    request->error_code = 0;
    request->error_message = NULL;
    request->error_headers = NULL;
    request->headers = NULL;
    request->time0 = null_time;
    request->time1 = null_time;
    request->request = NULL;
    request->next = NULL;
    return request;
}

void
httpDestroyRequest(HTTPRequestPtr request)
{
    if(request->object)
        releaseObject(request->object);
    if(request->condition)
        httpDestroyCondition(request->condition);
    releaseAtom(request->via);
    assert(request->ohandler == NULL);
    releaseAtom(request->error_message);
    releaseAtom(request->headers);
    releaseAtom(request->error_headers);
    assert(request->request == NULL);
    assert(request->next == NULL);
    free(request);
}

void
httpQueueRequest(HTTPConnectionPtr connection, HTTPRequestPtr request)
{
    assert(request->next == NULL && request->connection == NULL);
    request->connection = connection;
    if(connection->request_last) {
        assert(connection->request);
        connection->request_last->next = request;
        connection->request_last = request;
    } else {
        assert(!connection->request_last);
        connection->request = request;
        connection->request_last = request;
    }
}

HTTPRequestPtr
httpDequeueRequest(HTTPConnectionPtr connection)
{
    HTTPRequestPtr request = connection->request;
    if(request) {
        assert(connection->request_last);
        connection->request = request->next;
        if(!connection->request) connection->request_last = NULL;
        request->next = NULL;
    }
    return request;
}

HTTPConditionPtr 
httpMakeCondition()
{
    HTTPConditionPtr condition;
    condition = malloc(sizeof(HTTPConditionRec));
    if(condition == NULL)
        return NULL;
    condition->ims = -1;
    condition->inms = -1;
    condition->im = NULL;
    condition->inm = NULL;
    condition->ifrange = NULL;
    return condition;
}

void
httpDestroyCondition(HTTPConditionPtr condition)
{
    if(condition->inm)
        free(condition->inm);
    if(condition->im)
        free(condition->im);
    if(condition->ifrange)
        free(condition->ifrange);
    free(condition);
}
        
int
httpCondition(ObjectPtr object, HTTPConditionPtr condition)
{
    int rc = CONDITION_MATCH;

    assert(!(object->flags & OBJECT_INITIAL));

    if(!condition) return CONDITION_MATCH;

    if(condition->ims >= 0) {
        if(condition->ims < object->last_modified)
            return rc;
        else
            rc = CONDITION_NOT_MODIFIED;
    }

    if(condition->inms >= 0) {
        if(condition->inms >= object->last_modified)
            return rc;
        else
            rc = CONDITION_FAILED;
    }

    if(condition->inm) {
        if(!object->etag || strcmp(object->etag, condition->inm) != 0)
            return rc;
        else
            rc = CONDITION_NOT_MODIFIED;
    }

    if(condition->im) {
        if(!object->etag || strcmp(object->etag, condition->im) != 0)
            rc = CONDITION_FAILED;
        else
            return rc;
    }

    return rc;
}

int
httpWriteErrorHeaders(char *buf, int size, int offset, int do_body,
                      int code, AtomPtr message, int close, AtomPtr headers,
                      char *url, int url_len, char *etag)
{
    int n, m;
    char *body;

    assert(code != 0);

    if(code != 304) {
        body = get_chunk();
        if(!body) {
            do_log(L_ERROR, "Couldn't allocate body buffer.\n");
            return -1;
        }
        m = snnprintf(body, 0, CHUNK_SIZE,
                      "<!DOCTYPE HTML PUBLIC "
                      "\"-//W3C//DTD HTML 4.01 Transitional//EN\" "
                      "\"http://www.w3.org/TR/html4/loose.dtd\">"
                      "\n<html><head>"
                      "\n<title>Proxy error: %3d %s.</title>"
                      "\n</head><body>"
                      "\n<p>The proxy on %s:%d "
                      "encountered the following error",
                      code, atomString(message), 
                      proxyName->string, proxyPort);
        if(url_len > 0) {
            m = snnprintf(body, m, CHUNK_SIZE,
                          " while fetching <strong>");
            m = htmlString(body, m, CHUNK_SIZE, url, url_len);
            m = snnprintf(body, m, CHUNK_SIZE, "</strong>");
        }
        
        m = snnprintf(body, m, CHUNK_SIZE,
                      ":<br>"
                      "\n<strong>%3d %s</strong></p>"
                      "\n</body></html>\r\n",
                      code, atomString(message));
        if(m <= 0 || m >= CHUNK_SIZE) {
            do_log(L_ERROR, "Couldn't write error body.\n");
            dispose_chunk(body);
            return -1;
        }
    } else {
        body = NULL;
        m = 0;
    }

    n = snnprintf(buf, 0, size,
                 "HTTP/1.1 %3d %s"
                 "\r\nConnection: %s"
                 "\r\nDate: ",
                  code, atomString(message),
                  close ? "close" : "keep-alive");
    n = format_time(buf, n, size, current_time.tv_sec);
    if(code != 304) {
        n = snnprintf(buf, n, size,
                      "\r\nContent-Type: text/html"
                      "\r\nContent-Length: %d", m);
    } else {
        if(etag)
            n = snnprintf(buf, n, size, "\r\nETag: \"%s\"", etag);
    }

    if(code != 304 && code != 412) {
        n = snnprintf(buf, n, size,
                      "\r\nExpires: 0"
                      "\r\nCache-Control: no-cache"
                      "\r\nPragma: no-cache");
    }

    if(headers)
        n = snnprint_n(buf, n, size,
                      headers->string, headers->length);

    n = snnprintf(buf, n, size, "\r\n\r\n");

    if(n < 0 || n >= size) {
        do_log(L_ERROR, "Couldn't write error.\n");
        dispose_chunk(body);
        return -1;
    }

    if(code != 304 && do_body) {
        if(m > 0) memcpy(buf + n, body, m);
        n += m;
    }

    if(body)
        dispose_chunk(body);

    return n;
}

