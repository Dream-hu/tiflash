#include <Common/DNSCache.h>
#include <Interpreters/Context.h>
#include <Storages/Transaction/BackgroundService.h>
#include <Storages/Transaction/KVStore.h>
#include <Storages/Transaction/RaftCommandResult.h>
#include <Storages/Transaction/RegionRangeKeys.h>
#include <Storages/Transaction/SchemaSyncer.h>
#include <Storages/Transaction/TMTContext.h>
#include <Storages/Transaction/TiDBSchemaSyncer.h>
#include <pingcap/pd/MockPDClient.h>

namespace DB
{

TMTContext::TMTContext(Context & context_, const std::vector<std::string> & addrs, const std::string & learner_key,
    const std::string & learner_value, const std::unordered_set<std::string> & ignore_databases_, const std::string & kvstore_path,
    ::TiDB::StorageEngine engine_, bool disable_bg_flush_)
    : context(context_),
      kvstore(std::make_shared<KVStore>(kvstore_path)),
      region_table(context),
      background_service(nullptr),
      cluster(addrs.size() == 0 ? std::make_shared<pingcap::kv::Cluster>()
                                : std::make_shared<pingcap::kv::Cluster>(addrs, learner_key, learner_value)),
      ignore_databases(ignore_databases_),
      schema_syncer(addrs.size() == 0 ? std::static_pointer_cast<SchemaSyncer>(std::make_shared<TiDBSchemaSyncer<true>>(cluster))
                                      : std::static_pointer_cast<SchemaSyncer>(std::make_shared<TiDBSchemaSyncer<false>>(cluster))),
      engine(engine_),
      disable_bg_flush(disable_bg_flush_)
{}

void TMTContext::restore()
{
    kvstore->restore([&](pingcap::kv::RegionVerID id) -> IndexReaderPtr { return this->createIndexReader(id); });
    region_table.restore();
    initialized = true;

    background_service = std::make_unique<BackgroundService>(*this);
}

KVStorePtr & TMTContext::getKVStore() { return kvstore; }

const KVStorePtr & TMTContext::getKVStore() const { return kvstore; }

ManagedStorages & TMTContext::getStorages() { return storages; }

const ManagedStorages & TMTContext::getStorages() const { return storages; }

RegionTable & TMTContext::getRegionTable() { return region_table; }

const RegionTable & TMTContext::getRegionTable() const { return region_table; }

BackgroundService & TMTContext::getBackgroundService() { return *background_service; }

const BackgroundService & TMTContext::getBackgroundService() const { return *background_service; }

Context & TMTContext::getContext() { return context; }

bool TMTContext::isInitialized() const { return initialized; }

SchemaSyncerPtr TMTContext::getSchemaSyncer() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return schema_syncer;
}

void TMTContext::setSchemaSyncer(SchemaSyncerPtr rhs)
{
    std::lock_guard<std::mutex> lock(mutex);
    schema_syncer = rhs;
}

pingcap::pd::ClientPtr TMTContext::getPDClient() const { return cluster->pd_client; }

IndexReaderPtr TMTContext::createIndexReader(pingcap::kv::RegionVerID region_version_id) const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (cluster->pd_client->isMock())
    {
        return nullptr;
    }
    return std::make_shared<IndexReader>(cluster, region_version_id);
}

const std::unordered_set<std::string> & TMTContext::getIgnoreDatabases() const { return ignore_databases; }

} // namespace DB