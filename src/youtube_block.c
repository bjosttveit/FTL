#include <regex.h>
#include <string.h>
#include <stdio.h>

#include "FTL.h"
#include "youtube_block.h"
#include "datastructure.h"

bool isYoutubeDomain(const char *domain)
{
    static regex_t *regex = 0;

    if (regex == 0)
    {
        regex = malloc(sizeof(regex_t));
        regcomp(regex, "^r[0-9]+(\\.|-+)(sn-)?[a-z0-9]+(-[a-z0-9]+)*\\.googlevideo\\.com$", REG_EXTENDED);
    }

    int result = regexec(regex, domain, 0, NULL, 0);

    return result == REG_NOERROR;
}

static youtubeClient *initClient(const char *domain, const int clientID, queriesData *query)
{
    youtubeClient *currentClient = malloc(sizeof(youtubeClient));

    currentClient->clientID = clientID;

    char *approvedDomain = malloc(strlen(domain) * sizeof(char));
    strcpy(approvedDomain, domain);
    currentClient->lastApprovedDomain = approvedDomain;

    currentClient->lastRequestTime = query->timestamp;

    currentClient->nextClient = 0;

    return currentClient;
}

bool check_youtube_ad(const char *domain, const int clientID, queriesData *query, DNSCacheData *dns_cache,
                      const char **blockingreason, unsigned char *new_status)
{

    /*FILE *fp;
    fp = fopen("/home/bjosttveit/FTL/yt.log", "w+");
    fprintf(fp, "%d", clientID);
    fclose(fp);*/

    dns_cache->blocking_status = UNKNOWN_BLOCKED;

    static youtubeClient *yClients = 0;

    if (yClients == 0)
    {
        //First time any client connects, create the list
        yClients = initClient(domain, clientID, query);
    }
    else
    {

        youtubeClient *currentClient = yClients;

        while (currentClient != 0)
        {
            if (currentClient->clientID == clientID)
            {
                //This is the client for my request
                break;
            }
            currentClient = currentClient->nextClient;
        }

        if (currentClient == 0)
        {
            //First time this client connects, add it to list
            currentClient = initClient(domain, clientID, query);
            currentClient->nextClient = yClients;
            yClients = currentClient;
        }
        else
        {
            //This is a new request from an existing client
            bool block = false;

            if ((double)(query->timestamp - currentClient->lastRequestTime) <= 10.0)
            {
                block = strcmp(currentClient->lastApprovedDomain, domain) != 0;
            }
            else
            {
                char *approvedDomain = malloc(strlen(domain) * sizeof(char));
                strcpy(approvedDomain, domain);
                currentClient->lastApprovedDomain = approvedDomain;
            }

            currentClient->lastRequestTime = query->timestamp;
            if (block)
            {
                *new_status = QUERY_BLACKLIST;
                *blockingreason = "youtube video ad";
                return true;
            }
        }
    }
    return false;
}