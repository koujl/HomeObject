#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <homestore/replication_service.hpp>
#include "hs_homeobject.hpp"
#include "replication_state_machine.hpp"

using namespace homestore;
namespace homeobject {
PGError toPgError(ReplServiceError const& e) {
    switch (e) {
    case ReplServiceError::BAD_REQUEST:
        [[fallthrough]];
    case ReplServiceError::CANCELLED:
        [[fallthrough]];
    case ReplServiceError::CONFIG_CHANGING:
        [[fallthrough]];
    case ReplServiceError::SERVER_ALREADY_EXISTS:
        [[fallthrough]];
    case ReplServiceError::SERVER_IS_JOINING:
        [[fallthrough]];
    case ReplServiceError::SERVER_IS_LEAVING:
        [[fallthrough]];
    case ReplServiceError::RESULT_NOT_EXIST_YET:
        [[fallthrough]];
    case ReplServiceError::TERM_MISMATCH:
    case ReplServiceError::NOT_IMPLEMENTED:
        return PGError::INVALID_ARG;
    case ReplServiceError::NOT_LEADER:
        return PGError::NOT_LEADER;
    case ReplServiceError::CANNOT_REMOVE_LEADER:
        return PGError::UNKNOWN_PEER;
    case ReplServiceError::TIMEOUT:
        return PGError::TIMEOUT;
    case ReplServiceError::SERVER_NOT_FOUND:
        return PGError::UNKNOWN_PG;
    case ReplServiceError::NO_SPACE_LEFT:
        return PGError::NO_SPACE_LEFT;
    case ReplServiceError::DRIVE_WRITE_ERROR:
        return PGError::DRIVE_WRITE_ERROR;
    case ReplServiceError::RETRY_REQUEST:
        return PGError::RETRY_REQUEST;
    /* TODO: enable this after add erro type to homestore
    case ReplServiceError::CRC_MISMATCH:
        return PGError::CRC_MISMATCH;
     */
    case ReplServiceError::OK:
        DEBUG_ASSERT(false, "Should not process OK!");
        [[fallthrough]];
    case ReplServiceError::FAILED:
        return PGError::UNKNOWN;
    }
    return PGError::UNKNOWN;
}

[[maybe_unused]] static homestore::ReplDev& pg_repl_dev(PG const& pg) {
    return *(static_cast< HSHomeObject::HS_PG const& >(pg).repl_dev_);
}

PGManager::NullAsyncResult HSHomeObject::_create_pg(PGInfo&& pg_info, std::set< peer_id_t > const& peers) {
    auto pg_id = pg_info.id;
    if (auto lg = std::shared_lock(_pg_lock); _pg_map.end() != _pg_map.find(pg_id)) return folly::Unit();

    if (pg_info.size == 0) {
        LOGW("Not supported to create empty PG, pg_id {}, pg_size {}", pg_id, pg_info.size);
        return folly::makeUnexpected(PGError::INVALID_ARG);
    }

    const auto most_avail_num_chunks = chunk_selector()->most_avail_num_chunks();
    const auto chunk_size = chunk_selector()->get_chunk_size();
    const auto needed_num_chunks = sisl::round_down(pg_info.size, chunk_size) / chunk_size;
    if (needed_num_chunks > most_avail_num_chunks) {
        LOGW("No enough space to create pg, pg_id {}, needed_num_chunks {}, most_avail_num_chunks {}", pg_id,
             needed_num_chunks, most_avail_num_chunks);
        return folly::makeUnexpected(PGError::NO_SPACE_LEFT);
    }

    pg_info.chunk_size = chunk_size;
    pg_info.replica_set_uuid = boost::uuids::random_generator()();
    return hs_repl_service()
        .create_repl_dev(pg_info.replica_set_uuid, peers)
        .via(executor_)
        .thenValue([this, pg_info = std::move(pg_info)](auto&& v) mutable -> PGManager::NullAsyncResult {
            if (v.hasError()) { return folly::makeUnexpected(toPgError(v.error())); }
            // we will write a PGHeader across the raft group and when it is committed
            // all raft members will create PGinfo and index table for this PG.

            // FIXME:https://github.com/eBay/HomeObject/pull/136#discussion_r1470504271
            return do_create_pg(v.value(), std::move(pg_info));
        });
}

PGManager::NullAsyncResult HSHomeObject::do_create_pg(cshared< homestore::ReplDev > repl_dev, PGInfo&& pg_info) {
    auto serailized_pg_info = serialize_pg_info(pg_info);
    auto info_size = serailized_pg_info.size();

    auto req = repl_result_ctx< PGManager::NullResult >::make(info_size, 0);
    req->header()->msg_type = ReplicationMessageType::CREATE_PG_MSG;
    req->header()->payload_size = info_size;
    req->header()->payload_crc = crc32_ieee(init_crc32, r_cast< const uint8_t* >(serailized_pg_info.data()), info_size);
    req->header()->seal();
    std::memcpy(req->header_extn(), serailized_pg_info.data(), info_size);

    // replicate this create pg message to all raft members of this group
    repl_dev->async_alloc_write(req->header_buf(), sisl::blob{}, sisl::sg_list{}, req);
    return req->result().deferValue([req](auto const& e) -> PGManager::NullAsyncResult {
        if (!e) { return folly::makeUnexpected(e.error()); }
        return folly::Unit();
    });
}

void HSHomeObject::on_create_pg_message_commit(int64_t lsn, sisl::blob const& header,
                                               shared< homestore::ReplDev > repl_dev,
                                               cintrusive< homestore::repl_req_ctx >& hs_ctx) {
    repl_result_ctx< PGManager::NullResult >* ctx{nullptr};
    if (hs_ctx && hs_ctx->is_proposer()) {
        ctx = boost::static_pointer_cast< repl_result_ctx< PGManager::NullResult > >(hs_ctx).get();
    }

    auto const* msg_header = r_cast< ReplicationMessageHeader const* >(header.cbytes());

    if (msg_header->corrupted()) {
        LOGE("create PG message header is corrupted , lsn:{}; header: {}", lsn, msg_header->to_string());
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(PGError::CRC_MISMATCH)); }
        return;
    }

    auto serailized_pg_info_buf = header.cbytes() + sizeof(ReplicationMessageHeader);
    const auto serailized_pg_info_size = header.size() - sizeof(ReplicationMessageHeader);

    if (crc32_ieee(init_crc32, serailized_pg_info_buf, serailized_pg_info_size) != msg_header->payload_crc) {
        // header & value is inconsistent;
        LOGE("create PG message header is inconsistent with value, lsn:{}", lsn);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(PGError::CRC_MISMATCH)); }
        return;
    }

    auto pg_info = deserialize_pg_info(serailized_pg_info_buf, serailized_pg_info_size);
    auto pg_id = pg_info.id;
    if (auto lg = std::shared_lock(_pg_lock); _pg_map.end() != _pg_map.find(pg_id)) {
        LOGW("PG already exists, lsn:{}, pg_id {}", lsn, pg_id);
        if (ctx) { ctx->promise_.setValue(folly::Unit()); }
        return;
    }

    auto local_chunk_size = chunk_selector()->get_chunk_size();
    if (pg_info.chunk_size != local_chunk_size) {
        LOGE("Chunk sizes are inconsistent, leader_chunk_size={}, local_chunk_size={}", pg_info.chunk_size,
             local_chunk_size);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(PGError::UNKNOWN)); }
        return;
    }

    // select chunks for pg
    auto const num_chunk = chunk_selector()->select_chunks_for_pg(pg_id, pg_info.size);
    if (!num_chunk.has_value()) {
        LOGW("Failed to select chunks for pg {}", pg_id);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(PGError::NO_SPACE_LEFT)); }
        return;
    }
    auto chunk_ids = chunk_selector()->get_pg_chunks(pg_id);
    if (chunk_ids == nullptr) {
        LOGW("Failed to get pg chunks, pg_id {}", pg_id);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(PGError::NO_SPACE_LEFT)); }
        return;
    }
    // create index table and pg
    // TODO create index table during create shard.
    auto index_table = create_index_table();
    auto uuid_str = boost::uuids::to_string(index_table->uuid());

    auto hs_pg = std::make_unique< HS_PG >(std::move(pg_info), std::move(repl_dev), index_table, chunk_ids);
    std::scoped_lock lock_guard(index_lock_);
    RELEASE_ASSERT(index_table_pg_map_.count(uuid_str) == 0, "duplicate index table found");
    index_table_pg_map_[uuid_str] = PgIndexTable{pg_id, index_table};

    LOGI("Index table created for pg {} uuid {}", pg_id, uuid_str);
    hs_pg->index_table_ = index_table;
    // Add to index service, so that it gets cleaned up when index service is shutdown.
    homestore::hs()->index_service().add_index_table(index_table);
    add_pg_to_map(std::move(hs_pg));
    if (ctx) ctx->promise_.setValue(folly::Unit());
}

PGManager::NullAsyncResult HSHomeObject::_replace_member(pg_id_t pg_id, peer_id_t const& old_member_id,
                                                         PGMember const& new_member, uint32_t commit_quorum) {

    group_id_t group_id;
    {
        auto lg = std::shared_lock(_pg_lock);
        auto iter = _pg_map.find(pg_id);
        if (iter == _pg_map.end()) return folly::makeUnexpected(PGError::UNKNOWN_PG);
        auto& repl_dev = pg_repl_dev(*iter->second);

        if (!repl_dev.is_leader() && commit_quorum == 0) {
            // Only leader can replace a member
            return folly::makeUnexpected(PGError::NOT_LEADER);
        }
        group_id = repl_dev.group_id();
    }

    LOGI("PG replace member initated member_out={} member_in={}", boost::uuids::to_string(old_member_id),
         boost::uuids::to_string(new_member.id));

    homestore::replica_member_info in_replica, out_replica;
    out_replica.id = old_member_id;
    in_replica.id = new_member.id;
    in_replica.priority = new_member.priority;
    std::strncpy(in_replica.name, new_member.name.data(), new_member.name.size());
    in_replica.name[new_member.name.size()] = '\0';

    return hs_repl_service()
        .replace_member(group_id, out_replica, in_replica, commit_quorum)
        .via(executor_)
        .thenValue([this](auto&& v) mutable -> PGManager::NullAsyncResult {
            if (v.hasError()) { return folly::makeUnexpected(toPgError(v.error())); }
            return folly::Unit();
        });
}

void HSHomeObject::on_pg_replace_member(homestore::group_id_t group_id, const replica_member_info& member_out,
                                        const replica_member_info& member_in) {
    auto lg = std::shared_lock(_pg_lock);
    for (const auto& iter : _pg_map) {
        auto& pg = iter.second;
        if (pg_repl_dev(*pg).group_id() == group_id) {
            // Remove the old member and add the new member
            auto hs_pg = static_cast< HSHomeObject::HS_PG* >(pg.get());
            pg->pg_info_.members.erase(PGMember(member_out.id));
            pg->pg_info_.members.emplace(PGMember(member_in.id, member_in.name, member_in.priority));

            uint32_t i{0};
            pg_members* sb_members = hs_pg->pg_sb_->get_pg_members_mutable();
            for (auto const& m : pg->pg_info_.members) {
                sb_members[i].id = m.id;
                std::strncpy(sb_members[i].name, m.name.c_str(), std::min(m.name.size(), pg_members::max_name_len));
                sb_members[i].priority = m.priority;
                ++i;
            }

            // Update the latest membership info to pg superblk.
            hs_pg->pg_sb_.write();
            LOGI("PG replace member done member_out={} member_in={}", boost::uuids::to_string(member_out.id),
                 boost::uuids::to_string(member_in.id));
            return;
        }
    }

    LOGE("PG replace member failed member_out={} member_in={}", boost::uuids::to_string(member_out.id),
         boost::uuids::to_string(member_in.id));
}

void HSHomeObject::add_pg_to_map(unique< HS_PG > hs_pg) {
    RELEASE_ASSERT(hs_pg->pg_info_.replica_set_uuid == hs_pg->repl_dev_->group_id(),
                   "PGInfo replica set uuid mismatch with ReplDev instance for {}",
                   boost::uuids::to_string(hs_pg->pg_info_.replica_set_uuid));
    auto lg = std::scoped_lock(_pg_lock);
    auto id = hs_pg->pg_info_.id;
    auto [it1, _] = _pg_map.try_emplace(id, std::move(hs_pg));
    RELEASE_ASSERT(_pg_map.end() != it1, "Unknown map insert error!");
}

std::string HSHomeObject::serialize_pg_info(const PGInfo& pginfo) {
    nlohmann::json j;
    j["pg_info"]["pg_id_t"] = pginfo.id;
    j["pg_info"]["pg_size"] = pginfo.size;
    j["pg_info"]["chunk_size"] = pginfo.chunk_size;
    j["pg_info"]["repl_uuid"] = boost::uuids::to_string(pginfo.replica_set_uuid);

    nlohmann::json members_j{};
    for (auto const& member : pginfo.members) {
        nlohmann::json member_j;
        member_j["member_id"] = boost::uuids::to_string(member.id);
        member_j["name"] = member.name;
        member_j["priority"] = member.priority;
        members_j.push_back(member_j);
    }
    j["pg_info"]["members"] = members_j;
    return j.dump();
}

PGInfo HSHomeObject::deserialize_pg_info(const unsigned char* json_str, size_t size) {
    auto pg_json = nlohmann::json::parse(json_str, json_str + size);

    PGInfo pg_info(pg_json["pg_info"]["pg_id_t"].get< pg_id_t >());
    pg_info.size = pg_json["pg_info"]["pg_size"].get< uint64_t >();
    pg_info.chunk_size = pg_json["pg_info"]["chunk_size"].get< uint64_t >();
    pg_info.replica_set_uuid = boost::uuids::string_generator()(pg_json["pg_info"]["repl_uuid"].get< std::string >());

    for (auto const& m : pg_json["pg_info"]["members"]) {
        auto uuid_str = m["member_id"].get< std::string >();
        PGMember member(boost::uuids::string_generator()(uuid_str));
        member.name = m["name"].get< std::string >();
        member.priority = m["priority"].get< int32_t >();
        pg_info.members.emplace(std::move(member));
    }
    return pg_info;
}

void HSHomeObject::on_pg_meta_blk_found(sisl::byte_view const& buf, void* meta_cookie) {
    homestore::superblk< pg_info_superblk > pg_sb(_pg_meta_name);
    pg_sb.load(buf, meta_cookie);

    auto v = hs_repl_service().get_repl_dev(pg_sb->replica_set_uuid);
    if (v.hasError()) {
        // TODO: We need to raise an alert here, since without pg repl_dev all operations on that pg will fail
        LOGE("open_repl_dev for group_id={} has failed", boost::uuids::to_string(pg_sb->replica_set_uuid));
        return;
    }
    auto pg_id = pg_sb->id;
    std::vector< chunk_num_t > p_chunk_ids(pg_sb->get_chunk_ids(), pg_sb->get_chunk_ids() + pg_sb->num_chunks);
    bool set_pg_chunks_res = chunk_selector_->recover_pg_chunks(pg_id, std::move(p_chunk_ids));
    RELEASE_ASSERT(set_pg_chunks_res, "Failed to set pg={} chunks", pg_id);
    auto uuid_str = boost::uuids::to_string(pg_sb->index_table_uuid);
    auto hs_pg = std::make_unique< HS_PG >(std::move(pg_sb), std::move(v.value()));
    // During PG recovery check if index is already recoverd else
    // add entry in map, so that index recovery can update the PG.
    std::scoped_lock lg(index_lock_);
    auto it = index_table_pg_map_.find(uuid_str);
    RELEASE_ASSERT(it != index_table_pg_map_.end(), "IndexTable should be recovered before PG");
    hs_pg->index_table_ = it->second.index_table;
    it->second.pg_id = pg_id;

    add_pg_to_map(std::move(hs_pg));
}

void HSHomeObject::on_pg_meta_blk_recover_completed(bool success) { chunk_selector_->recover_per_dev_chunk_heap(); }

PGInfo HSHomeObject::HS_PG::pg_info_from_sb(homestore::superblk< pg_info_superblk > const& sb) {
    PGInfo pginfo{sb->id};
    const pg_members* sb_members = sb->get_pg_members();
    for (uint32_t i{0}; i < sb->num_members; ++i) {
        pginfo.members.emplace(sb_members[i].id, std::string(sb_members[i].name), sb_members[i].priority);
    }
    pginfo.size = sb->pg_size;
    pginfo.replica_set_uuid = sb->replica_set_uuid;
    return pginfo;
}

HSHomeObject::HS_PG::HS_PG(PGInfo info, shared< homestore::ReplDev > rdev, shared< BlobIndexTable > index_table,
                           std::shared_ptr< const std::vector< chunk_num_t > > pg_chunk_ids) :
        PG{std::move(info)},
        pg_sb_{_pg_meta_name},
        repl_dev_{std::move(rdev)},
        index_table_{std::move(index_table)},
        metrics_{*this} {
    RELEASE_ASSERT(pg_chunk_ids != nullptr, "PG chunks null");
    const uint32_t num_chunks = pg_chunk_ids->size();
    pg_sb_.create(sizeof(pg_info_superblk) - sizeof(char) + pg_info_.members.size() * sizeof(pg_members) +
                  num_chunks * sizeof(homestore::chunk_num_t));
    pg_sb_->id = pg_info_.id;
    pg_sb_->num_members = pg_info_.members.size();
    pg_sb_->num_chunks = num_chunks;
    pg_sb_->pg_size = pg_info_.size;
    pg_sb_->replica_set_uuid = repl_dev_->group_id();
    pg_sb_->index_table_uuid = index_table_->uuid();
    pg_sb_->active_blob_count = 0;
    pg_sb_->tombstone_blob_count = 0;
    pg_sb_->total_occupied_blk_count = 0;
    uint32_t i{0};
    pg_members* pg_sb_members = pg_sb_->get_pg_members_mutable();
    for (auto const& m : pg_info_.members) {
        pg_sb_members[i].id = m.id;
        std::strncpy(pg_sb_members[i].name, m.name.c_str(), std::min(m.name.size(), pg_members::max_name_len));
        pg_sb_members[i].priority = m.priority;
        ++i;
    }
    chunk_num_t* pg_sb_chunk_ids = pg_sb_->get_chunk_ids_mutable();
    for (i = 0; i < num_chunks; ++i) {
        pg_sb_chunk_ids[i] = pg_chunk_ids->at(i);
    }
    pg_sb_.write();
}

HSHomeObject::HS_PG::HS_PG(homestore::superblk< HSHomeObject::pg_info_superblk >&& sb,
                           shared< homestore::ReplDev > rdev) :
        PG{pg_info_from_sb(sb)}, pg_sb_{std::move(sb)}, repl_dev_{std::move(rdev)}, metrics_{*this} {
    durable_entities_.blob_sequence_num = pg_sb_->blob_sequence_num;
    durable_entities_.active_blob_count = pg_sb_->active_blob_count;
    durable_entities_.tombstone_blob_count = pg_sb_->tombstone_blob_count;
    durable_entities_.total_occupied_blk_count = pg_sb_->total_occupied_blk_count;
}

uint32_t HSHomeObject::HS_PG::total_shards() const { return shards_.size(); }

uint32_t HSHomeObject::HS_PG::open_shards() const {
    return std::count_if(shards_.begin(), shards_.end(), [](auto const& s) { return s->is_open(); });
}

bool HSHomeObject::_get_stats(pg_id_t id, PGStats& stats) const {
    auto lg = std::shared_lock(_pg_lock);
    auto it = _pg_map.find(id);
    if (_pg_map.end() == it) { return false; }
    auto const& pg = it->second;
    auto hs_pg = static_cast< HS_PG* >(pg.get());
    auto const blk_size = hs_pg->repl_dev_->get_blk_size();

    stats.id = hs_pg->pg_info_.id;
    stats.replica_set_uuid = hs_pg->pg_info_.replica_set_uuid;
    stats.num_members = hs_pg->pg_info_.members.size();
    stats.total_shards = hs_pg->total_shards();
    stats.open_shards = hs_pg->open_shards();
    stats.leader_id = hs_pg->repl_dev_->get_leader_id();
    stats.num_active_objects = hs_pg->durable_entities().active_blob_count.load(std::memory_order_relaxed);
    stats.num_tombstone_objects = hs_pg->durable_entities().tombstone_blob_count.load(std::memory_order_relaxed);

    auto const replication_status = hs_pg->repl_dev_->get_replication_status();
    for (auto const& m : hs_pg->pg_info_.members) {
        auto last_commit_lsn = 0ul;
        auto last_succ_resp_us = 0ul;
        // replication_status can be empty in follower
        for (auto const& r : replication_status) {
            if (r.id_ == m.id) {
                last_commit_lsn = r.replication_idx_;
                last_succ_resp_us = r.last_succ_resp_us_;
                break;
            }
        }
        stats.members.emplace_back(std::make_tuple(m.id, m.name, last_commit_lsn, last_succ_resp_us));
    }

    stats.avail_open_shards = chunk_selector()->avail_num_chunks(hs_pg->pg_info_.id);
    stats.avail_bytes = chunk_selector()->avail_blks(hs_pg->pg_info_.id) * blk_size;
    stats.used_bytes = hs_pg->durable_entities().total_occupied_blk_count.load(std::memory_order_relaxed) * blk_size;

    return true;
}

void HSHomeObject::_get_pg_ids(std::vector< pg_id_t >& pg_ids) const {
    {
        auto lg = std::shared_lock(_pg_lock);
        for (auto& [id, _] : _pg_map) {
            pg_ids.push_back(id);
        }
    }
}

} // namespace homeobject
