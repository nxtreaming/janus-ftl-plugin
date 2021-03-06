/**
 * @file RestServiceConnection.h
 * @author Hayden McAfee (hayden@outlook.com)
 * @version 0.1
 * @date 2020-08-09
 *
 * @copyright Copyright (c) 2020 Hayden McAfee
 *
 */

#pragma once

#include "ServiceConnection.h"
#include "../Utilities/JanssonPtr.h"

#include <httplib.h>
#include <string>

/**
 * @brief
 * RestServiceConnection is a service connection implementation for a generic REST
 * API server.
 */
class RestServiceConnection : public ServiceConnection
{
public:
    /* Constructor/Destructor */
    RestServiceConnection(
        std::string hostname,
        uint16_t port,
        bool useHttps,
        std::string pathBase,
        std::string authToken);

    // ServiceConnection
    void Init() override;
    Result<std::vector<std::byte>> GetHmacKey(ftl_channel_id_t channelId) override;
    Result<ftl_stream_id_t> StartStream(ftl_channel_id_t channelId) override;
    Result<ServiceResponse> UpdateStreamMetadata(ftl_stream_id_t streamId,
        StreamMetadata metadata) override;
    Result<void> EndStream(ftl_stream_id_t streamId) override;
    Result<void> SendJpegPreviewImage(ftl_stream_id_t streamId,
        std::vector<uint8_t> jpegData) override;

private:
    /* Private members */
    const int MAX_RETRIES = 5;
    const int TIME_BETWEEN_RETRIES_MS = 3000;
    const int DEFAULT_SOCKET_RECEIVE_TIMEOUT_SEC = 1;
    std::string baseUri;
    std::string hostname;
    std::string pathBase;
    std::string authToken;

    /* Private methods */
    std::unique_ptr<httplib::Client> getHttpClientWithAuth();
    std::string relativeToAbsolutePath(std::string relativePath);
    httplib::Result runGetRequest(std::string path);
    httplib::Result runPostRequest(std::string path, JsonPtr body = nullptr,
        httplib::MultipartFormDataItems fileData = httplib::MultipartFormDataItems());
    JsonPtr decodeRestResponse(const httplib::Result& result);
};
