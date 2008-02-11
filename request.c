/*
Copyright (c) 2007 by Juliusz Chroboczek

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

#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include "babel.h"
#include "util.h"
#include "neighbour.h"
#include "request.h"
#include "message.h"
#include "network.h"
#include "filter.h"

int request_resend_time = 0;
struct request *recorded_requests = NULL;

static int
request_match(struct request *request,
              const unsigned char *prefix, unsigned char plen)
{
    return request->plen == plen && memcmp(request->prefix, prefix, 16) == 0;
}

struct request *
find_request(const unsigned char *prefix, unsigned char plen,
             struct request **previous_return)
{
    struct request *request, *previous;

    previous = NULL;
    request = recorded_requests;
    while(request) {
        if(request_match(request, prefix, plen)) {
            if(previous_return)
                *previous_return = previous;
            return request;
        }
        previous = request;
        request = request->next;
    }

    return NULL;
}

int
record_request(const unsigned char *prefix, unsigned char plen,
               unsigned short seqno, unsigned short router_hash,
               struct network *network, int resend)
{
    struct request *request;
    unsigned int ifindex = network ? network->ifindex : 0;

    if(input_filter(NULL, prefix, plen, NULL, ifindex) >= INFINITY ||
       output_filter(NULL, prefix, plen, ifindex) >= INFINITY)
        return 0;

    request = find_request(prefix, plen, NULL);
    if(request) {
        if(request->resend && resend)
            request->resend = MIN(request->resend, resend);
        else if(resend)
            request->resend = resend;
        request->time = now.tv_sec;
        request_resend_time = MIN(request_resend_time,
                                  request->time + request->resend);
        if(request->router_hash == router_hash &&
           seqno_compare(request->seqno, seqno) > 0) {
            return 0;
        } else {
            request->router_hash = router_hash;
            request->seqno = seqno;
            if(request->network != network)
                request->network = NULL;
            return 1;
        }
    } else {
        request = malloc(sizeof(struct request));
        if(request == NULL)
            return -1;
        memcpy(request->prefix, prefix, 16);
        request->plen = plen;
        request->seqno = seqno;
        request->router_hash = router_hash;
        request->network = network;
        request->time = now.tv_sec;
        request->resend = resend;
        if(resend)
            request_resend_time = MIN(request_resend_time, now.tv_sec + resend);
        request->next = recorded_requests;
        recorded_requests = request;
        return 1;
    }
}

int
satisfy_request(const unsigned char *prefix, unsigned char plen,
                unsigned short seqno, unsigned short router_hash,
                struct network *network)
{
    struct request *request, *previous;

    request = find_request(prefix, plen, &previous);
    if(request == NULL)
        return 0;

    if(network != NULL && request->network != network)
        return 0;

    if(request->router_hash != router_hash ||
       seqno_compare(request->seqno, seqno) <= 0) {
        if(previous == NULL)
            recorded_requests = request->next;
        else
            previous->next = request->next;
        free(request);
        recompute_request_resend_time();
        return 1;
    }

    return 0;
}

void
expire_requests()
{
    struct request *request, *previous;
    int recompute = 0;

    previous = NULL;
    request = recorded_requests;
    while(request) {
        if(request->time < now.tv_sec - REQUEST_TIMEOUT) {
            if(previous == NULL) {
                recorded_requests = request->next;
                free(request);
                request = recorded_requests;
            } else {
                previous->next = request->next;
                free(request);
                request = previous->next;
            }
            recompute = 1;
        } else {
            request = request->next;
        }
    }
    if(recompute)
        recompute_request_resend_time();
}

int
recompute_request_resend_time()
{
    struct request *request;
    int resend = 0;

    request = recorded_requests;
    while(request) {
        if(request->resend) {
            if(resend)
                resend = MIN(resend, request->time + request->resend);
            else
                resend = request->time + request->resend;
        }
        request = request->next;
    }

    request_resend_time = resend;
    return resend;
}

void
resend_requests()
{
    struct request *request;

    request = recorded_requests;
    while(request) {
        if(request->resend && now.tv_sec >= request->time + request->resend) {
            send_request(NULL, request->prefix, request->plen, 127,
                         request->seqno, request->router_hash);
            request->resend = 2 * request->resend;
        }
        request = request->next;
    }
    recompute_request_resend_time();
}
