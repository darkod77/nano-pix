#include <nano/lib/threading.hpp>
#include <nano/lib/timer.hpp>
#include <nano/secure/store.hpp>

nano::representative_visitor::representative_visitor (nano::transaction const & transaction_a, nano::store & store_a) :
	transaction (transaction_a),
	store (store_a),
	result (0)
{
}

void nano::representative_visitor::compute (nano::block_hash const & hash_a)
{
	current = hash_a;
	while (result.is_zero ())
	{
		auto block (store.block.get (transaction, current));
		debug_assert (block != nullptr);
		block->visit (*this);
	}
}

void nano::representative_visitor::send_block (nano::send_block const & block_a)
{
	current = block_a.previous ();
}

void nano::representative_visitor::receive_block (nano::receive_block const & block_a)
{
	current = block_a.previous ();
}

void nano::representative_visitor::open_block (nano::open_block const & block_a)
{
	result = block_a.hash ();
}

void nano::representative_visitor::change_block (nano::change_block const & block_a)
{
	result = block_a.hash ();
}

void nano::representative_visitor::state_block (nano::state_block const & block_a)
{
	result = block_a.hash ();
}

nano::read_transaction::read_transaction (std::unique_ptr<nano::read_transaction_impl> read_transaction_impl) :
	impl (std::move (read_transaction_impl))
{
}

void * nano::read_transaction::get_handle () const
{
	return impl->get_handle ();
}

void nano::read_transaction::reset () const
{
	impl->reset ();
}

void nano::read_transaction::renew () const
{
	impl->renew ();
}

void nano::read_transaction::refresh () const
{
	reset ();
	renew ();
}

nano::write_transaction::write_transaction (std::unique_ptr<nano::write_transaction_impl> write_transaction_impl) :
	impl (std::move (write_transaction_impl))
{
	/*
	 * For IO threads, we do not want them to block on creating write transactions.
	 */
	debug_assert (nano::thread_role::get () != nano::thread_role::name::io);
}

void * nano::write_transaction::get_handle () const
{
	return impl->get_handle ();
}

void nano::write_transaction::commit ()
{
	impl->commit ();
}

void nano::write_transaction::renew ()
{
	impl->renew ();
}

void nano::write_transaction::refresh ()
{
	impl->commit ();
	impl->renew ();
}

bool nano::write_transaction::contains (nano::tables table_a) const
{
	return impl->contains (table_a);
}

nano::account nano::block_store::account (nano::block const & block) const
{
	debug_assert (block.has_sideband ());
	nano::account result (block.account ());
	if (result.is_zero ())
	{
		result = block.sideband ().account;
	}
	debug_assert (!result.is_zero ());
	return result;
}

nano::account nano::block_store::account (nano::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block = get (transaction, hash);
	debug_assert (block != nullptr);
	return account (*block);
}

nano::uint128_t nano::block_store::balance (nano::block const & block) const
{
	nano::uint128_t result;
	switch (block.type ())
	{
		case nano::block_type::open:
		case nano::block_type::receive:
		case nano::block_type::change:
			result = block.sideband ().balance.number ();
			break;
		case nano::block_type::send:
		case nano::block_type::state:
			result = block.balance ().number ();
			break;
		case nano::block_type::invalid:
		case nano::block_type::not_a_block:
			release_assert (false);
			break;
	}
	return result;
}

nano::uint128_t nano::block_store::balance (nano::transaction const & transaction, nano::block_hash const & hash) const
{
	auto block = get (transaction, hash);
	debug_assert (block != nullptr);
	return balance (*block);
}

// clang-format off
nano::store::store (
	nano::block_store & block_store_a,
	nano::frontier_store & frontier_store_a,
	nano::account_store & account_store_a,
	nano::pending_store & pending_store_a,
	nano::online_weight_store & online_weight_store_a,
	nano::pruned_store & pruned_store_a,
	nano::peer_store & peer_store_a,
	nano::confirmation_height_store & confirmation_height_store_a,
	nano::final_vote_store & final_vote_store_a,
	nano::version_store & version_store_a
) :
	block (block_store_a),
	frontier (frontier_store_a),
	account (account_store_a),
	pending (pending_store_a),
	online_weight (online_weight_store_a),
	pruned (pruned_store_a),
	peer (peer_store_a),
	confirmation_height (confirmation_height_store_a),
	final_vote (final_vote_store_a),
	version (version_store_a)
{
}
// clang-format on

/**
 * If using a different store version than the latest then you may need
 * to modify some of the objects in the store to be appropriate for the version before an upgrade.
 */
void nano::store::initialize (nano::write_transaction const & transaction_a, nano::ledger_cache & ledger_cache_a, nano::ledger_constants & constants)
{
	debug_assert (constants.genesis->has_sideband ());
	debug_assert (account.begin (transaction_a) == account.end ());
	auto hash_l (constants.genesis->hash ());
	block.put (transaction_a, hash_l, *constants.genesis);
	++ledger_cache_a.block_count;
	confirmation_height.put (transaction_a, constants.genesis->account (), nano::confirmation_height_info{ 1, constants.genesis->hash () });
	++ledger_cache_a.cemented_count;
	ledger_cache_a.final_votes_confirmation_canary = (constants.final_votes_canary_account == constants.genesis->account () && 1 >= constants.final_votes_canary_height);
	account.put (transaction_a, constants.genesis->account (), { hash_l, constants.genesis->account (), constants.genesis->hash (), std::numeric_limits<nano::uint128_t>::max (), nano::seconds_since_epoch (), 1, nano::epoch::epoch_0 });
	++ledger_cache_a.account_count;
	ledger_cache_a.rep_weights.representation_put (constants.genesis->account (), std::numeric_limits<nano::uint128_t>::max ());
	frontier.put (transaction_a, hash_l, constants.genesis->account ());
}

std::optional<nano::account_info> nano::account_store::get (const nano::transaction & transaction, const nano::account & account)
{
	nano::account_info info;
	bool error = get (transaction, account, info);
	if (!error)
	{
		return info;
	}
	else
	{
		return std::nullopt;
	}
}

std::optional<nano::confirmation_height_info> nano::confirmation_height_store::get (const nano::transaction & transaction, const nano::account & account)
{
	nano::confirmation_height_info info;
	bool error = get (transaction, account, info);
	if (!error)
	{
		return info;
	}
	else
	{
		return std::nullopt;
	}
}
