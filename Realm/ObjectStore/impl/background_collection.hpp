////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#ifndef REALM_BACKGROUND_COLLECTION_HPP
#define REALM_BACKGROUND_COLLECTION_HPP

#include "collection_notifications.hpp"

#include <realm/group_shared.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace realm {
class Realm;

namespace _impl {
class CollectionChangeBuilder : public CollectionChangeIndices {
public:
    CollectionChangeBuilder(CollectionChangeBuilder const&) = default;
    CollectionChangeBuilder(CollectionChangeBuilder&&) = default;
    CollectionChangeBuilder& operator=(CollectionChangeBuilder const&) = default;
    CollectionChangeBuilder& operator=(CollectionChangeBuilder&&) = default;

    CollectionChangeBuilder(IndexSet deletions = {},
                            IndexSet insertions = {},
                            IndexSet modification = {},
                            std::vector<Move> moves = {});

    static CollectionChangeBuilder calculate(std::vector<size_t> const& old_rows,
                                             std::vector<size_t> const& new_rows,
                                             std::function<bool (size_t)> row_did_change,
                                             bool sort);

    void merge(CollectionChangeBuilder&&);
    void clean_up_stale_moves();

    void insert(size_t ndx, size_t count=1, bool track_moves=true);
    void modify(size_t ndx);
    void erase(size_t ndx);
    void move_over(size_t ndx, size_t last_ndx, bool track_moves=true);
    void clear(size_t old_size);
    void move(size_t from, size_t to);

    void parse_complete();

private:
    std::unordered_map<size_t, size_t> m_move_mapping;

    void verify();

};
struct ListChangeInfo {
    size_t table_ndx;
    size_t row_ndx;
    size_t col_ndx;
    CollectionChangeBuilder* changes;
};

struct TransactionChangeInfo {
    std::vector<bool> table_modifications_needed;
    std::vector<bool> table_moves_needed;
    std::vector<ListChangeInfo> lists;
    std::vector<CollectionChangeBuilder> tables;

    bool row_did_change(Table const& table, size_t row_ndx, int depth = 0) const;
};

// A base class for a notifier that keeps a collection up to date and/or
// generates detailed change notifications on a background thread. This manages
// most of the lifetime-management issues related to sharing an object between
// the worker thread and the collection on the target thread, along with the
// thread-safe callback collection.
class BackgroundCollection {
public:
    BackgroundCollection(std::shared_ptr<Realm>);
    virtual ~BackgroundCollection();

    // ------------------------------------------------------------------------
    // Public API for the collections using this to get notifications:

    // Stop receiving notifications from this background worker
    // This must be called in the destructor of the collection
    void unregister() noexcept;

    // Add a callback to be called each time the collection changes
    // This can only be called from the target collection's thread
    // Returns a token which can be passed to remove_callback()
    size_t add_callback(CollectionChangeCallback callback);
    // Remove a previously added token. The token is no longer valid after
    // calling this function and must not be used again. This function can be
    // called from any thread.
    void remove_callback(size_t token);

    // ------------------------------------------------------------------------
    // API for RealmCoordinator to manage running things and calling callbacks

    Realm* get_realm() const noexcept { return m_realm.get(); }

    // Get the SharedGroup version which this collection can attach to (if it's
    // in handover mode), or can deliver to (if it's been handed over to the BG worker alredad)
    SharedGroup::VersionID version() const noexcept { return m_sg_version; }

    // Release references to all core types
    // This is called on the worker thread to ensure that non-thread-safe things
    // can be destroyed on the correct thread, even if the last reference to the
    // BackgroundCollection is released on a different thread
    virtual void release_data() noexcept = 0;

    // Call each of the currently registered callbacks, if there have been any
    // changes since the last time each of those callbacks was called
    void call_callbacks();

    bool is_alive() const noexcept;

    // Attach the handed-over query to `sg`. Must not be already attaged to a SharedGroup.
    void attach_to(SharedGroup& sg);
    // Create a new query handover object and stop using the previously attached
    // SharedGroup
    void detach();

    // Set `info` as the new ChangeInfo that will be populated by the next
    // transaction advance, and register all required information in it
    void add_required_change_info(TransactionChangeInfo& info);

    // Do all background work needed to prepare the notification
    // Always called on a single thread, without any locks held
    virtual void run(SharedGroup&) = 0;

    // Update all cached state so that calculating the next changeset can be
    // done from this version, but do not actually do the work needed to prepare
    // the handover object or calculate changes
    // Always called on a single thread, without any locks held
    virtual void skip(SharedGroup&) = 0;

    // Actually populate the fields used for delivery with the data calculated in run()
    // Called on the same thread as run(), with a lock guarding this and deliver()
    void prepare_handover();

    // Called on the target thread, with a lock guarding this and prepare_handover()
    bool deliver(SharedGroup&, std::exception_ptr);

    SharedGroup::VersionID skip_to_version() const { return m_skip_to_version; }
    void skip_to_version(SharedGroup::VersionID version) { m_skip_to_version = version; }

    bool is_for_current_thread() const { return m_thread_id == std::this_thread::get_id(); }

protected:
    bool have_callbacks() const noexcept { return m_have_callbacks; }
    void add_changes(CollectionChangeBuilder change) { m_accumulated_changes.merge(std::move(change)); }
    void set_table(Table const& table);
    std::unique_lock<std::mutex> lock_target();

private:
    virtual void do_attach_to(SharedGroup&) = 0;
    virtual void do_detach_from(SharedGroup&) = 0;
    virtual void do_prepare_handover() = 0;
    virtual bool do_deliver(SharedGroup&) { return true; }
    virtual bool do_add_required_change_info(TransactionChangeInfo&) { return true; }

    const std::thread::id m_thread_id = std::this_thread::get_id();

    mutable std::mutex m_realm_mutex;
    std::shared_ptr<Realm> m_realm;

    SharedGroup::VersionID m_skip_to_version;
    SharedGroup::VersionID m_sg_version;
    SharedGroup* m_sg = nullptr;

    std::exception_ptr m_error;
    CollectionChangeBuilder m_accumulated_changes;
    CollectionChangeIndices m_changes_to_deliver;

    // Tables which this collection needs change information for
    std::vector<size_t> m_relevant_tables;

    struct Callback {
        CollectionChangeCallback fn;
        size_t token;
        bool initial_delivered;
    };

    // Currently registered callbacks and a mutex which must always be held
    // while doing anything with them or m_callback_index
    std::mutex m_callback_mutex;
    std::vector<Callback> m_callbacks;

    // Cached value for if m_callbacks is empty, needed to avoid deadlocks in
    // run() due to lock-order inversion between m_callback_mutex and m_target_mutex
    // It's okay if this value is stale as at worst it'll result in us doing
    // some extra work.
    std::atomic<bool> m_have_callbacks = {false};

    // Iteration variable for looping over callbacks
    // remove_callback() updates this when needed
    size_t m_callback_index = npos;

    CollectionChangeCallback next_callback();
};

} // namespace _impl
} // namespace realm

#endif /* REALM_BACKGROUND_COLLECTION_HPP */
