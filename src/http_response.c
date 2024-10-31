#include <stddef.h>
#include <http_response.h>
#include <span.h>

HL_RESULT http_response_init(http_response_t* response, http_request_t* request, span_t code, span_t reason_phrase)
{
    HL_RESULT result;

    if (request == NULL || response == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        response->request = request;
        response->code = code;
        response->reason_phrase = reason_phrase;
        response->headers = NULL;
        response->body = SPAN_EMPTY;

        result = HL_RESULT_OK;
    }

    return result;
}

span_t http_response_get_code(http_response_t response)
{
    return response.code;
}

span_t http_response_get_reason_phrase(http_response_t response)
{
    return response.reason_phrase;
}

span_t http_response_get_http_version(http_response_t response)
{
    return response.request->version;
}

HL_RESULT http_response_set_headers(http_response_t* response, http_headers_t* headers)
{
    HL_RESULT result;

    if (response == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        response->headers = headers;

        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_response_get_headers(http_response_t response, http_headers_t** headers)
{
    HL_RESULT result;

    if (headers == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        *headers = response.headers;

        result = HL_RESULT_OK;
    }

    return result;
}

HL_RESULT http_response_set_body(http_response_t* response, span_t body)
{
    // uint8_t raw_buffer[64];
    // buffer buffer = buffer_from_array(raw_buffer);

    // body->open(body);
    // int l = body->get_data(buffer, body);
    // body->close(body);

    // send(raw_buffer, l)

    HL_RESULT result;

    if (response == NULL)
    {
        result = HL_RESULT_INVALID_ARG;
    }
    else
    {
        response->body = body;

        result = HL_RESULT_OK;
    }

    return result;
}

void http_response_deinit(http_response_t* request)
{
    if (request != NULL)
    {
        
    }
}
