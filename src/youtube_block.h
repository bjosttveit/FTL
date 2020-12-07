#include <stdbool.h>
#include "datastructure.h"

#ifndef YOUTUBE_BLOCK_H
#define YOUTUBE_BLOCK_H

typedef struct youtubeClient youtubeClient;

struct youtubeClient
{
    int clientID;
    time_t lastRequestTime;
    char *lastApprovedDomain;
    struct youtubeClient *nextClient;
};

bool check_youtube_ad(const char *domain, const int clientID, queriesData *query, DNSCacheData *dns_cache,
                      const char **blockingreason, unsigned char *new_status);

bool isYoutubeDomain(const char *domain);

#endif