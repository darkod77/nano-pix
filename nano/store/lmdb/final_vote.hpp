#pragma once

#include <nano/store/final_vote.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano::store::lmdb
{
class component;
}
namespace nano::store::lmdb
{
class final_vote : public nano::store::final_vote
{
private:
	nano::store::lmdb::component & store;

public:
	explicit final_vote (nano::store::lmdb::component & store);
	bool put (store::write_transaction const & transaction_a, nano::qualified_root const & root_a, nano::block_hash const & hash_a) override;
	std::optional<nano::block_hash> get (store::transaction const & transaction_a, nano::qualified_root const & qualified_root_a) override;
	void del (store::write_transaction const & transaction_a, nano::qualified_root const & root_a) override;
	size_t count (store::transaction const & transaction_a) const override;
	void clear (store::write_transaction const & transaction_a) override;
	iterator begin (store::transaction const & transaction_a, nano::qualified_root const & root_a) const override;
	iterator begin (store::transaction const & transaction_a) const override;
	iterator end (store::transaction const & transaction_a) const override;
	void for_each_par (std::function<void (store::read_transaction const &, iterator, iterator)> const & action_a) const override;

	/**
	 * Maps root to block hash for generated final votes.
	 * nano::qualified_root -> nano::block_hash
	 */
	MDB_dbi final_votes_handle{ 0 };
};
} // namespace nano::store::lmdb
