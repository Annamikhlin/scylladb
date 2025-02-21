/*
 * Copyright (C) 2023-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "dht/token.hh"
#include "utils/small_vector.hh"
#include "locator/host_id.hh"
#include "dht/i_partitioner_fwd.hh"
#include "schema/schema_fwd.hh"
#include "utils/chunked_vector.hh"
#include "utils/hash.hh"

#include <boost/range/adaptor/transformed.hpp>
#include <seastar/core/reactor.hh>
#include <seastar/util/log.hh>

#include <vector>

namespace locator {

extern seastar::logger tablet_logger;

using token = dht::token;

// Identifies tablet within the scope of a single tablet_map,
// which has a scope of (table_id, token metadata version).
// Different tablets of different tables can have the same tablet_id.
// Different tablets in subsequent token metadata version can have the same tablet_id.
// When splitting a tablet, one of the new tablets (in the new token metadata version)
// will have the same tablet_id as the old one.
enum class tablet_id : size_t;

struct tablet_replica {
    host_id host;
    shard_id shard;

    bool operator==(const tablet_replica&) const = default;
};

std::ostream& operator<<(std::ostream&, tablet_id);
std::ostream& operator<<(std::ostream&, const tablet_replica&);

using tablet_replica_set = utils::small_vector<tablet_replica, 3>;

/// Stores information about a single tablet.
struct tablet_info {
    tablet_replica_set replicas;

    bool operator==(const tablet_info&) const = default;
};

/// Used for storing tablet state transition during topology changes.
/// Describes transition of a single tablet.
struct tablet_transition_info {
    tablet_replica_set next;
    tablet_replica pending_replica; // Optimization (next - tablet_info::replicas)

    bool operator==(const tablet_transition_info&) const = default;
};

/// Stores information about tablets of a single table.
///
/// The map contains a constant number of tablets, tablet_count().
/// Each tablet has an associated tablet_info, and an optional tablet_transition_info.
/// Any given token is owned by exactly one tablet in this map.
///
/// A tablet map describes the whole ring, it cannot contain a partial mapping.
/// This means that the following sequence is always valid:
///
///    tablet_map& tmap = ...;
///    dht::token t = ...;
///    tablet_id id = tmap.get_tablet_id(t);
///    tablet_info& info = tmap.get_tablet_info(id);
///
/// A tablet_id obtained from an instance of tablet_map is valid for that instance only.
class tablet_map {
public:
    using tablet_container = utils::chunked_vector<tablet_info>;
private:
    // The implementation assumes that _tablets.size() is a power of 2:
    //
    //   _tablets.size() == 1 << _log2_tablets
    //
    tablet_container _tablets;
    size_t _log2_tablets; // log_2(_tablets.size())
    std::unordered_map<tablet_id, tablet_transition_info> _transitions;
public:
    /// Constructs a tablet map.
    ///
    /// \param tablet_count The desired tablets to allocate. Must be a power of two.
    explicit tablet_map(size_t tablet_count);

    /// Returns tablet_id of a tablet which owns a given token.
    tablet_id get_tablet_id(token) const;

    /// Returns tablet_info associated with a given tablet.
    /// The given id must belong to this instance.
    const tablet_info& get_tablet_info(tablet_id) const;

    /// Returns a pointer to tablet_transition_info associated with a given tablet.
    /// If there is no transition for a given tablet, returns nullptr.
    /// \throws std::logic_error If the given id does not belong to this instance.
    const tablet_transition_info* get_tablet_transition_info(tablet_id) const;

    /// Returns the largest token owned by a given tablet.
    /// \throws std::logic_error If the given id does not belong to this instance.
    dht::token get_last_token(tablet_id id) const;

    /// Returns the smallest token owned by a given tablet.
    /// \throws std::logic_error If the given id does not belong to this instance.
    dht::token get_first_token(tablet_id id) const;

    /// Returns token_range which contains all tokens owned by a given tablet and only such tokens.
    /// \throws std::logic_error If the given id does not belong to this instance.
    dht::token_range get_token_range(tablet_id id) const;

    /// Returns the id of the first tablet.
    tablet_id first_tablet() const {
        return tablet_id(0);
    }

    /// Returns the id of the last tablet.
    tablet_id last_tablet() const {
        return tablet_id(tablet_count() - 1);
    }

    /// Returns the id of a tablet which follows a given tablet in the ring,
    /// or disengaged optional if the given tablet is the last one.
    std::optional<tablet_id> next_tablet(tablet_id t) const {
        if (t == last_tablet()) {
            return std::nullopt;
        }
        return tablet_id(size_t(t) + 1);
    }

    /// Returns shard id which is a replica for a given tablet on a given host.
    /// If there is no replica on a given host, returns nullopt.
    /// If the topology is transitional, also considers the new replica set.
    /// The old replica set is preferred in case of ambiguity.
    std::optional<shard_id> get_shard(tablet_id, host_id) const;

    const tablet_container& tablets() const {
        return _tablets;
    }

    const auto& transitions() const {
        return _transitions;
    }

    /// Returns an iterable range over tablet_id:s which includes all tablets in token ring order.
    auto tablet_ids() const {
        return boost::irange<size_t>(0, tablet_count()) | boost::adaptors::transformed([] (size_t i) {
            return tablet_id(i);
        });
    }

    size_t tablet_count() const {
        return _tablets.size();
    }

    /// Returns tablet_info associated with the tablet which owns a given token.
    const tablet_info& get_tablet_info(token t) const {
        return get_tablet_info(get_tablet_id(t));
    }

    size_t external_memory_usage() const;

    bool operator==(const tablet_map&) const = default;
public:
    void set_tablet(tablet_id, tablet_info);
    void set_tablet_transition_info(tablet_id, tablet_transition_info);

    // Destroys gently.
    // The tablet map is not usable after this call and should be destroyed.
    future<> clear_gently();
    friend std::ostream& operator<<(std::ostream&, const tablet_map&);
private:
    void check_tablet_id(tablet_id) const;
};

/// Holds information about all tablets in the cluster.
///
/// When this instance is obtained via token_metadata_ptr, it is immutable
/// (represents a snapshot) and references obtained through this are guaranteed
/// to remain valid as long as the containing token_metadata_ptr is held.
///
/// Copy constructor can be invoked across shards.
class tablet_metadata {
public:
    // FIXME: Make cheap to copy.
    // We want both immutability and cheap updates, so we should use
    // hierarchical data structure with shared pointers and copy-on-write.
    // Currently we have immutability but updates require full copy.
    //
    // Also, currently the copy constructor is invoked across shards, which precludes
    // using shared pointers. We should change that and use a foreign_ptr<> to
    // hold immutable tablet_metadata which lives on shard 0 only.
    // See storage_service::replicate_to_all_cores().
    using table_to_tablet_map = std::unordered_map<table_id, tablet_map>;
private:
    table_to_tablet_map _tablets;
public:
    const tablet_map& get_tablet_map(table_id id) const;
    const table_to_tablet_map& all_tables() const { return _tablets; }
    size_t external_memory_usage() const;
public:
    void set_tablet_map(table_id, tablet_map);
    tablet_map& get_tablet_map(table_id id);
    future<> clear_gently();
public:
    bool operator==(const tablet_metadata&) const = default;
    friend std::ostream& operator<<(std::ostream&, const tablet_metadata&);
};

}

namespace std {

template<>
struct hash<locator::tablet_replica> {
    size_t operator()(const locator::tablet_replica& r) const {
        return utils::hash_combine(
                std::hash<locator::host_id>()(r.host),
                std::hash<shard_id>()(r.shard));
    }
};

}
