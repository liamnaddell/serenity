/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/TraversableNavigable.h>

namespace Web::HTML {

TraversableNavigable::TraversableNavigable() = default;

TraversableNavigable::~TraversableNavigable() = default;

void TraversableNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& entry : m_session_history_entries)
        visitor.visit(entry);
}

static OrderedHashTable<TraversableNavigable*>& user_agent_top_level_traversable_set()
{
    static OrderedHashTable<TraversableNavigable*> set;
    return set;
}

struct BrowsingContextAndDocument {
    JS::NonnullGCPtr<HTML::BrowsingContext> browsing_context;
    JS::NonnullGCPtr<DOM::Document> document;
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-browsing-context
static WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_top_level_browsing_context_and_document(Page& page)
{
    // 1. Let group and document be the result of creating a new browsing context group and document.
    auto [group, document] = TRY(BrowsingContextGroup::create_a_new_browsing_context_group_and_document(page));

    // 2. Return group's browsing context set[0] and document.
    return BrowsingContextAndDocument { **group->browsing_context_set().begin(), document };
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
WebIDL::ExceptionOr<JS::NonnullGCPtr<TraversableNavigable>> TraversableNavigable::create_a_new_top_level_traversable(Page& page, JS::GCPtr<HTML::BrowsingContext> opener, String target_name)
{
    auto& vm = Bindings::main_thread_vm();

    // 1. Let document be null.
    JS::GCPtr<DOM::Document> document = nullptr;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener) {
        document = TRY(create_a_new_top_level_browsing_context_and_document(page)).document;
    }

    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = TRY(BrowsingContext::create_a_new_auxiliary_browsing_context_and_document(page, *opener)).document;
    }

    // 4. Let documentState be a new document state, with
    auto document_state = vm.heap().allocate_without_realm<DocumentState>();

    // document: document
    document_state->set_document(document);

    // navigable target name: targetName
    document_state->set_navigable_target_name(target_name);

    // 5. Let traversable be a new traversable navigable.
    auto traversable = vm.heap().allocate_without_realm<TraversableNavigable>();

    // 6. Initialize the navigable traversable given documentState.
    TRY_OR_THROW_OOM(vm, traversable->initialize_navigable(document_state, nullptr));

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    auto initial_history_entry = traversable->active_session_history_entry();
    VERIFY(initial_history_entry);

    // 8. Set initialHistoryEntry's step to 0.
    initial_history_entry->step = 0;

    // 9. Append initialHistoryEntry to traversable's session history entries.
    traversable->m_session_history_entries.append(*initial_history_entry);

    // FIXME: 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]

    // 11. Append traversable to the user agent's top-level traversable set.
    user_agent_top_level_traversable_set().set(traversable);

    // 12. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-fresh-top-level-traversable
WebIDL::ExceptionOr<JS::NonnullGCPtr<TraversableNavigable>> TraversableNavigable::create_a_fresh_top_level_traversable(Page& page, AK::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> initial_navigation_post_resource)
{
    // 1. Let traversable be the result of creating a new top-level traversable given null and the empty string.
    auto traversable = TRY(create_a_new_top_level_traversable(page, nullptr, {}));

    // 2. Navigate traversable to initialNavigationURL using traversable's active document, with documentResource set to initialNavigationPostResource.
    TRY(traversable->navigate(initial_navigation_url, *traversable->active_document(), initial_navigation_post_resource));

    // 3. Return traversable.
    return traversable;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#top-level-traversable
bool TraversableNavigable::is_top_level_traversable() const
{
    // A top-level traversable is a traversable navigable with a null parent.
    return parent() == nullptr;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#getting-all-used-history-steps
Vector<int> TraversableNavigable::get_all_used_history_steps() const
{
    // FIXME: 1. Assert: this is running within traversable's session history traversal queue.

    // 2. Let steps be an empty ordered set of non-negative integers.
    OrderedHashTable<int> steps;

    // 3. Let entryLists be the ordered set « traversable's session history entries ».
    Vector<Vector<JS::NonnullGCPtr<SessionHistoryEntry>>> entry_lists { session_history_entries() };

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto entry_list = entry_lists.take_first();

        // 1. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. Append entry's step to steps.
            steps.set(entry->step.get<int>());

            // 2. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state->nested_histories())
                entry_lists.append(nested_history.entries);
        }
    }

    // 5. Return steps, sorted.
    auto sorted_steps = steps.values();
    quick_sort(sorted_steps);
    return sorted_steps;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#clear-the-forward-session-history
void TraversableNavigable::clear_the_forward_session_history()
{
    // FIXME: 1. Assert: this is running within navigable's session history traversal queue.

    // 2. Let step be the navigable's current session history step.
    auto step = current_session_history_step();

    // 3. Let entryLists be the ordered set « navigable's session history entries ».
    Vector<Vector<JS::NonnullGCPtr<SessionHistoryEntry>>&> entry_lists;
    entry_lists.append(session_history_entries());

    // 4. For each entryList of entryLists:
    while (!entry_lists.is_empty()) {
        auto& entry_list = entry_lists.take_first();

        // 1. Remove every session history entry from entryList that has a step greater than step.
        entry_list.remove_all_matching([step](auto& entry) {
            return entry->step.template get<int>() > step;
        });

        // 2. For each entry of entryList:
        for (auto& entry : entry_list) {
            // 1. For each nestedHistory of entry's document state's nested histories, append nestedHistory's entries list to entryLists.
            for (auto& nested_history : entry->document_state->nested_histories()) {
                entry_lists.append(nested_history.entries);
            }
        }
    }
}

}
