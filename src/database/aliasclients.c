/* Pi-hole: A black hole for Internet advertisements
*  (c) 2020 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Super client table routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "../FTL.h"
#include "aliasclients.h"
#include "common.h"
// global counters variable
#include "../shmem.h"
// global config variable
#include "../config.h"
// logg()
#include "../log.h"
// calloc()
#include "../memory.h"
// getAliasclientIDfromIP()
#include "network-table.h"

bool create_aliasclients_table(void)
{
	// Create aliasclient table in the database
	SQL_bool("CREATE TABLE aliasclient (id INTEGER PRIMARY KEY NOT NULL, " \
	                                   "name TEXT NOT NULL, " \
	                                   "comment TEXT);");

	// Add aliasclient_id to network table
	SQL_bool("ALTER TABLE network ADD COLUMN aliasclient_id INTEGER;");

	// Update database version to 9
	if(!db_set_FTL_property(DB_VERSION, 9))
	{
		logg("create_aliasclients_table(): Failed to update database version!");
		return false;
	}

	return true;
}

// Recompute the alias-client's values
// We shouldn't do this too often as it iterates over all existing clients
static void recompute_aliasclient(const int aliasclientID)
{
	clientsData *aliasclient = getClient(aliasclientID, true);

	if(config.debug & DEBUG_ALIASCLIENTS)
	{
		logg("Recomputing alias-client \"%s\" (%s)...",
		     getstr(aliasclient->namepos), getstr(aliasclient->ippos));
	}

	// Reset this alias-client
	aliasclient->count = 0;
	aliasclient->blockedcount = 0;
	memset(aliasclient->overTime, 0, sizeof(aliasclient->overTime));

	// Loop over all existing clients to find which clients are associated to this one
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get pointer to client candidate
		const clientsData *client = getClient(clientID, true);
		// Skip invalid clients and alias-clients
		if(client == NULL || client->aliasclient)
			continue;

		// Skip clients that are not managed by this aliasclient
		if(client->aliasclient_id != aliasclientID)
		{
			if(config.debug & DEBUG_ALIASCLIENTS)
			{
				logg("Client \"%s\" (%s) NOT managed by this alias-client, skipping",
				     getstr(client->namepos), getstr(client->ippos));
			}
			continue;
		}

		// Debug logging
		if(config.debug & DEBUG_ALIASCLIENTS)
		{
			logg("Client \"%s\" (%s) IS  managed by this alias-client, adding counts",
					getstr(client->namepos), getstr(client->ippos));
		}

		// Add counts of this client to the alias-client
		aliasclient->count += client->count;
		aliasclient->blockedcount += client->blockedcount;
		for(int idx = 0; idx < OVERTIME_SLOTS; idx++)
			aliasclient->overTime[idx] += client->overTime[idx];
	}
}

// Store hostname of device identified by dbID
bool import_aliasclients(void)
{
	sqlite3_stmt *stmt = NULL;
	const char querystr[] = "SELECT id,name FROM aliasclient";

	int rc = sqlite3_prepare_v2(FTL_db, querystr, -1, &stmt, NULL);
	if(rc != SQLITE_OK)
	{
		logg("import_aliasclients() - SQL error prepare: %s", sqlite3_errstr(rc));
		return false;
	}

	// Loop until no further data is available
	int imported = 0;
	while((rc = sqlite3_step(stmt)) != SQLITE_DONE)
	{
		// Check if we ran into an error
		if(rc != SQLITE_ROW)
		{
			logg("import_aliasclients() - SQL error step: %s", sqlite3_errstr(rc));
			break;
		}

		// Get hardware address from database and store it as IP + MAC address of this client
		const int aliasclient_id = sqlite3_column_int(stmt, 0);

		// Create a new (super-)client
		char *aliasclient_str = NULL;
		if(asprintf(&aliasclient_str, "aliasclient-%i", aliasclient_id) < 10)
		{
			logg("Memory error in import_aliasclients()");
			return false;
		}

		// Try to open existing client
		const int clientID = findClientID(aliasclient_str, false, true);

		clientsData *client = getClient(clientID, true);
		client->new = false;

		// Reset counter
		client->count = 0;

		// Store intended name
		const char *name = (char*)sqlite3_column_text(stmt, 1);
		client->namepos = addstr(name);

		// This is a aliasclient
		client->aliasclient = true;
		client->aliasclient_id = aliasclient_id;

		// Debug logging
		if(config.debug & DEBUG_ALIASCLIENTS)
		{
			logg("Added alias-client \"%s\" (%s) with FTL ID %i", name, aliasclient_str, clientID);
		}

		free(aliasclient_str);
		imported++;
	}

	// Finalize statement
	if ((rc = sqlite3_finalize(stmt)) != SQLITE_OK)
	{
		logg("import_aliasclients() - SQL error finalize: %s", sqlite3_errstr(rc));
		return false;
	}

	logg("Imported %d alias-client%s", imported, (imported != 1) ? "s":"");

	return true;
}

static int get_aliasclient_ID(const clientsData *client)
{
	// Skip alias-clients themselves
	if(client->aliasclient)
		return -1;

	const char *clientIP = getstr(client->ippos);
	if(config.debug & DEBUG_ALIASCLIENTS)
	{
		logg("   Looking for the alias-client for client %s...",
		     clientIP);
	}

	// Get aliasclient ID from database (DB index)
	const int aliasclient_DBid = getAliasclientIDfromIP(clientIP);

	// Compare DB index for all alias-clients stored in FTL
	int aliasclientID = 0;
	for(; aliasclientID < counters->clients; aliasclientID++)
	{
		// Get pointer to alias client candidate
		const clientsData *alias_client = getClient(aliasclientID, true);

		// Skip clients that are not alias-clients
		if(!alias_client->aliasclient)
			continue;

		// Compare MAC address of the current client to the
		// alias client candidate's MAC address
		if(alias_client->aliasclient_id == aliasclient_DBid)
		{
			if(config.debug & DEBUG_ALIASCLIENTS)
			{
				logg("   -> \"%s\" (%s)",
				     getstr(alias_client->namepos),
				     getstr(alias_client->ippos));
			}

			return aliasclientID;
		}
	}

	if(config.debug & DEBUG_ALIASCLIENTS && aliasclientID == counters->clients)
	{
		logg("   -> not found");
	}

	// Not found
	return -1;
}

void reset_aliasclient(clientsData *client)
{
	// Skip alias-clients themselves
	if(client->aliasclient)
		return;

	// Find corresponding alias-client (if any)
	client->aliasclient_id = get_aliasclient_ID(client);

	// Skip if there is no responsible alias-client
	if(client->aliasclient_id == -1)
		return;

	// Recompute all values for this alias-client
	recompute_aliasclient(client->aliasclient_id);
}

// Return a list of clients linked to the current alias-client
// The first element contains the number of following IDs
int *get_aliasclient_list(const int aliasclientID)
{
	int count = 0;
	// Loop over all existing clients to count associated clients
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get pointer to client candidate
		const clientsData *client = getClient(clientID, true);
		// Skip invalid clients and those that are not managed by this aliasclient
		if(client == NULL || client->aliasclient_id != aliasclientID)
			continue;

		count++;
	}

	int *list = calloc(count + 1, sizeof(int));
	list[0] = count;

	// Loop over all existing clients to fill list of clients
	count = 0;
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get pointer to client candidate
		const clientsData *client = getClient(clientID, true);
		// Skip invalid clients and those that are not managed by this aliasclient
		if(client == NULL || client->aliasclient_id != aliasclientID)
			continue;

		list[++count] = clientID;
	}

	return list;
}

// Reimport alias-clients from database
// Note that this will always only change or add new clients. Alias-clients are
// removed by nulling them before importing new clients
void reimport_aliasclients(void)
{
	// Open pihole-FTL.db database file if needed
	const bool db_already_open = FTL_DB_avail();
	if(!db_already_open && !dbopen())
	{
		logg("reimport_aliasclients() - Failed to open DB");
		return;
	}
	// Loop over all existing alias-clients and set their counters to zero
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get pointer to client candidate
		clientsData *client = getClient(clientID, true);
		// Skip invalid and non-alias-clients
		if(client == NULL || !client->aliasclient)
			continue;

		// Reset this alias-client
		client->count = 0;
		client->blockedcount = 0;
		memset(client->overTime, 0, sizeof(client->overTime));
	}

	// Import aliasclients from database table
	import_aliasclients();

	if(!db_already_open)
		dbclose();

	// Recompute all alias-clients
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		// Get pointer to client candidate
		clientsData *client = getClient(clientID, true);
		// Skip invalid and alias-clients
		if(client == NULL || client->aliasclient)
			continue;

		reset_aliasclient(client);
	}
}