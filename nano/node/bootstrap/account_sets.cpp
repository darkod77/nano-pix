#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/bootstrap/account_sets.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>

#include <boost/range/iterator_range.hpp>

#include <algorithm>
#include <memory>
#include <vector>

/*
 * account_sets
 */

nano::bootstrap::account_sets::account_sets (nano::account_sets_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
}

void nano::bootstrap::account_sets::priority_up (nano::account const & account)
{
	if (account.is_zero ())
	{
		return;
	}

	if (!blocked (account))
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::prioritize);

		auto iter = priorities.get<tag_account> ().find (account);
		if (iter != priorities.get<tag_account> ().end ())
		{
			priorities.get<tag_account> ().modify (iter, [] (auto & val) {
				val.priority = std::min ((val.priority + account_sets::priority_increase), account_sets::priority_max);
			});
		}
		else
		{
			stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::priority_insert);
			priorities.get<tag_account> ().insert ({ account, account_sets::priority_initial });
			trim_overflow ();
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::prioritize_failed);
	}
}

void nano::bootstrap::account_sets::priority_down (nano::account const & account)
{
	if (account.is_zero ())
	{
		return;
	}

	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::deprioritize);

		auto priority_new = iter->priority / account_sets::priority_divide;
		if (priority_new <= account_sets::priority_cutoff)
		{
			stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::priority_erase_by_threshold);
			priorities.get<tag_account> ().erase (iter);
		}
		else
		{
			priorities.get<tag_account> ().modify (iter, [priority_new] (auto & val) {
				val.priority = priority_new;
			});
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::deprioritize_failed);
	}
}

void nano::bootstrap::account_sets::priority_set (nano::account const & account)
{
	if (account.is_zero ())
	{
		return;
	}

	if (!blocked (account))
	{
		auto iter = priorities.get<tag_account> ().find (account);
		if (iter == priorities.get<tag_account> ().end ())
		{
			stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::priority_insert);
			priorities.get<tag_account> ().insert ({ account, account_sets::priority_initial });
			trim_overflow ();
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::prioritize_failed);
	}
}

void nano::bootstrap::account_sets::block (nano::account const & account, nano::block_hash const & dependency)
{
	debug_assert (!account.is_zero ());

	stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::block);

	auto existing = priorities.get<tag_account> ().find (account);
	auto entry = (existing == priorities.get<tag_account> ().end ()) ? priority_entry{ account, 0 } : *existing;

	priorities.get<tag_account> ().erase (account);
	stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::priority_erase_by_blocking);

	blocking.get<tag_account> ().insert ({ entry, dependency });
	stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::blocking_insert);

	trim_overflow ();
}

void nano::bootstrap::account_sets::unblock (nano::account const & account, std::optional<nano::block_hash> const & hash)
{
	if (account.is_zero ())
	{
		return;
	}

	// Unblock only if the dependency is fulfilled
	auto existing = blocking.get<tag_account> ().find (account);
	if (existing != blocking.get<tag_account> ().end () && (!hash || existing->dependency == *hash))
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::unblock);

		debug_assert (priorities.get<tag_account> ().count (account) == 0);
		if (!existing->original_entry.account.is_zero ())
		{
			debug_assert (existing->original_entry.account == account);
			priorities.get<tag_account> ().insert (existing->original_entry);
		}
		else
		{
			priorities.get<tag_account> ().insert ({ account, account_sets::priority_initial });
		}
		blocking.get<tag_account> ().erase (account);

		trim_overflow ();
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::unblock_failed);
	}
}

void nano::bootstrap::account_sets::timestamp_set (const nano::account & account)
{
	debug_assert (!account.is_zero ());

	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		priorities.get<tag_account> ().modify (iter, [] (auto & entry) {
			entry.timestamp = std::chrono::steady_clock::now ();
		});
	}
}

void nano::bootstrap::account_sets::timestamp_reset (const nano::account & account)
{
	debug_assert (!account.is_zero ());

	auto iter = priorities.get<tag_account> ().find (account);
	if (iter != priorities.get<tag_account> ().end ())
	{
		priorities.get<tag_account> ().modify (iter, [] (auto & entry) {
			entry.timestamp = {};
		});
	}
}

void nano::bootstrap::account_sets::dependency_update (nano::block_hash const & hash, nano::account const & dependency_account)
{
	debug_assert (!dependency_account.is_zero ());

	auto [it, end] = blocking.get<tag_dependency> ().equal_range (hash);
	if (it != end)
	{
		while (it != end)
		{
			if (it->dependency_account != dependency_account)
			{
				stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::dependency_update);

				blocking.get<tag_dependency> ().modify (it++, [dependency_account] (auto & entry) {
					entry.dependency_account = dependency_account;
				});
			}
			else
			{
				++it;
			}
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::dependency_update_failed);
	}
}

void nano::bootstrap::account_sets::trim_overflow ()
{
	while (priorities.size () > config.priorities_max)
	{
		// Erase the oldest entry
		priorities.pop_front ();
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::priority_erase_overflow);
	}
	while (blocking.size () > config.blocking_max)
	{
		// Erase the oldest entry
		blocking.pop_front ();
		stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::blocking_erase_overflow);
	}
}

nano::account nano::bootstrap::account_sets::next_priority (std::function<bool (nano::account const &)> const & filter)
{
	if (priorities.empty ())
	{
		return { 0 };
	}

	auto const cutoff = std::chrono::steady_clock::now () - config.cooldown;

	for (auto const & entry : priorities.get<tag_priority> ())
	{
		if (entry.timestamp > cutoff)
		{
			continue;
		}
		if (!filter (entry.account))
		{
			continue;
		}
		return entry.account;
	}

	return { 0 };
}

nano::block_hash nano::bootstrap::account_sets::next_blocking (std::function<bool (nano::block_hash const &)> const & filter)
{
	if (blocking.empty ())
	{
		return { 0 };
	}

	// Scan all entries with unknown dependency account
	auto [begin, end] = blocking.get<tag_dependency_account> ().equal_range (nano::account{ 0 });
	for (auto const & entry : boost::make_iterator_range (begin, end))
	{
		debug_assert (entry.dependency_account.is_zero ());
		if (!filter (entry.dependency))
		{
			continue;
		}
		return entry.dependency;
	}

	return { 0 };
}

void nano::bootstrap::account_sets::sync_dependencies ()
{
	// Sample all accounts with a known dependency account (> account 0)
	auto begin = blocking.get<tag_dependency_account> ().upper_bound (nano::account{ 0 });
	auto end = blocking.get<tag_dependency_account> ().end ();

	for (auto const & entry : boost::make_iterator_range (begin, end))
	{
		debug_assert (!entry.dependency_account.is_zero ());

		if (priorities.size () >= config.priorities_max)
		{
			break;
		}

		if (!blocked (entry.dependency_account) && !prioritized (entry.dependency_account))
		{
			stats.inc (nano::stat::type::bootstrap_account_sets, nano::stat::detail::sync_dependencies);
			priority_set (entry.dependency_account);
		}
	}

	trim_overflow ();
}

bool nano::bootstrap::account_sets::blocked (nano::account const & account) const
{
	return blocking.get<tag_account> ().contains (account);
}

bool nano::bootstrap::account_sets::prioritized (nano::account const & account) const
{
	return priorities.get<tag_account> ().contains (account);
}

std::size_t nano::bootstrap::account_sets::priority_size () const
{
	return priorities.size ();
}

std::size_t nano::bootstrap::account_sets::blocked_size () const
{
	return blocking.size ();
}

bool nano::bootstrap::account_sets::priority_half_full () const
{
	return priorities.size () > config.priorities_max / 2;
}

bool nano::bootstrap::account_sets::blocked_half_full () const
{
	return blocking.size () > config.blocking_max / 2;
}

double nano::bootstrap::account_sets::priority (nano::account const & account) const
{
	if (!blocked (account))
	{
		auto existing = priorities.get<tag_account> ().find (account);
		if (existing != priorities.get<tag_account> ().end ())
		{
			return existing->priority;
		}
	}
	return 0.0;
}

auto nano::bootstrap::account_sets::info () const -> nano::bootstrap::account_sets::info_t
{
	return { blocking, priorities };
}

nano::container_info nano::bootstrap::account_sets::container_info () const
{
	// Count blocking entries with their dependency account unknown
	auto blocking_unknown = blocking.get<tag_dependency_account> ().count (nano::account{ 0 });

	nano::container_info info;
	info.put ("priorities", priorities);
	info.put ("blocking", blocking);
	info.put ("blocking_unknown", blocking_unknown);
	return info;
}
