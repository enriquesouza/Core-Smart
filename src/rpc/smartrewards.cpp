// Copyright (c) 2018-2020 The SmartCash developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "init.h"
#include "validation.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "smartrewards/rewards.h"
#include "smartrewards/rewardspayments.h"
#include "util.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"

#include <fstream>
#include <iomanip>
#include <univalue.h>


UniValue smartrewards(const UniValue& params, bool fHelp)
{
    std::function<double (CAmount)> format = [](CAmount a) {
        return a / COIN + ( double(a % COIN) / COIN );
    };

    std::string strCommand;
    if (params.size() >= 1) {
        strCommand = params[0].get_str();
    }

    if (fHelp  ||
        (
         strCommand != "current" && strCommand != "snapshot" && strCommand != "history" && strCommand != "check" && strCommand != "payouts"))
            throw std::runtime_error(
                "smartrewards \"command\"...\n"
                "Set of commands to execute smartrewards related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "\nAvailable commands:\n"
                "  current           - Print information about the current SmartReward cycle.\n"
                "  history           - Print the results of all past SmartReward cycles.\n"
                "  payouts  :round   - Print a list of all paid rewards in the past cycle :round\n"
                "  snapshot :round   - Print a list of all addresses with their balances from the end of the past cycle :round.\n"
                "  check :address    - Check the given :address for eligibility in the current rewards cycle.\n"
                );

    if( !fDebug && !prewards->IsSynced() )
        throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is not up to date.");

    TRY_LOCK(cs_rewardsdb, lockRewardsDb);

    if (!lockRewardsDb) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");
    }

    if (strCommand == "current")
    {
        UniValue obj(UniValue::VOBJ);

        TRY_LOCK(cs_rewardscache, cacheLocked);

        if(!cacheLocked) throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");

        const CSmartRewardRound *current = prewards->GetCurrentRound();

        if( !current->number ) throw JSONRPCError(RPC_DATABASE_ERROR, "No active reward round available yet.");

        obj.pushKV("rewards_cycle",current->number);
        obj.pushKV("start_blockheight",current->startBlockHeight);
        obj.pushKV("start_blocktime",current->startBlockTime);
        obj.pushKV("end_blockheight",current->endBlockHeight);
        obj.pushKV("end_blocktime",current->endBlockTime);
        obj.pushKV("eligible_addresses",current->eligibleEntries - current->disqualifiedEntries);
        obj.pushKV("eligible_smart",format(current->eligibleSmart - current->disqualifiedSmart));
        obj.pushKV("disqualified_addresses",current->disqualifiedEntries);
        obj.pushKV("disqualified_smart",format(current->disqualifiedSmart));
        obj.pushKV("estimated_rewards",format(current->rewards));
        obj.pushKV("estimated_percent",current->percent * 100.0);

        return obj;
    }

    if (strCommand == "history")
    {
        UniValue obj(UniValue::VARR);

        TRY_LOCK(cs_rewardscache, cacheLocked);

        if(!cacheLocked) throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");

        const CSmartRewardRoundMap* history = prewards->GetRewardRounds();

        int64_t nPayoutDelay = Params().GetConsensus().nRewardsPayoutStartDelay;

        if(!history->size()) throw JSONRPCError(RPC_DATABASE_ERROR, "No finished reward round available yet.");

        auto round = history->begin();

        while( round != history->end() ){

            UniValue roundObj(UniValue::VOBJ);

            roundObj.pushKV("rewards_cycle",round->second.number);
            roundObj.pushKV("start_blockheight",round->second.startBlockHeight);
            roundObj.pushKV("start_blocktime",round->second.startBlockTime);
            roundObj.pushKV("end_blockheight",round->second.endBlockHeight);
            roundObj.pushKV("end_blocktime",round->second.endBlockTime);
            roundObj.pushKV("eligible_addresses",((round->second.eligibleEntries - round->second.disqualifiedEntries)>0) ? (round->second.eligibleEntries - round->second.disqualifiedEntries) : 0);
            roundObj.pushKV("eligible_smart",format(((round->second.eligibleSmart - round->second.disqualifiedSmart) > 0) ? (round->second.eligibleSmart - round->second.disqualifiedSmart) : 0));
            roundObj.pushKV("disqualified_addresses",round->second.disqualifiedEntries);
            roundObj.pushKV("disqualified_smart",format(round->second.disqualifiedSmart));
            roundObj.pushKV("rewards",format(round->second.rewards));
            roundObj.pushKV("percent",round->second.percent * 100.0);

            UniValue payObj(UniValue::VOBJ);

            if( round->second.GetPayeeCount() ){

                payObj.pushKV("firstBlock", round->second.endBlockHeight + nPayoutDelay );
                payObj.pushKV("totalBlocks", round->second.GetRewardBlocks() );
                payObj.pushKV("lastBlock", round->second.GetLastRoundBlock() );
                payObj.pushKV("totalPayees", round->second.GetPayeeCount() );
                payObj.pushKV("blockPayees", round->second.nBlockPayees);
                payObj.pushKV("lastBlockPayees", round->second.GetPayeeCount() % round->second.nBlockPayees );
                payObj.pushKV("blockInterval", round->second.nBlockInterval);
            }else{
                payObj.pushKV("None","No payees were eligible for this round");
            }

            roundObj.pushKV("payouts", payObj);

            obj.push_back(roundObj);

            ++round;
        }

        return obj;
    }

    if(strCommand == "payouts")
    {
        TRY_LOCK(cs_rewardscache, cacheLocked);

        if(!cacheLocked) throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");

        const CSmartRewardRound *current = prewards->GetCurrentRound();

        if( !current->number ) throw JSONRPCError(RPC_DATABASE_ERROR, "No active reward round available yet.");

        int round = 0;
        std::string err = strprintf("Past SmartReward round required: 1 - %d ",current->number - 1 );

        if (params.size() != 2) throw JSONRPCError(RPC_INVALID_PARAMETER, err);

        try {
             int n = std::stoi(params[1].get_str());
             round = n;
        }
        catch (const std::invalid_argument& ia) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, err);
        }
        catch (const std::out_of_range& oor) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, err);
        }
        catch (...) {}

        if(round < 1 || round >= current->number) throw JSONRPCError(RPC_INVALID_PARAMETER, err);

        CSmartRewardResultEntryList payouts;

        if( !prewards->GetRewardPayouts(round,payouts) )
            throw JSONRPCError(RPC_DATABASE_ERROR, "Couldn't fetch the list from the database.");

        UniValue obj(UniValue::VARR);

        BOOST_FOREACH(CSmartRewardResultEntry payout, payouts) {

            UniValue addrObj(UniValue::VOBJ);
            addrObj.pushKV("address", payout.entry.id.ToString());
            addrObj.pushKV("reward", format(payout.reward));

            obj.push_back(addrObj);
        }

        return obj;
    }

    if(strCommand == "snapshot")
    {
        TRY_LOCK(cs_rewardscache, cacheLocked);

        if(!cacheLocked) throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");

        const CSmartRewardRound *current = prewards->GetCurrentRound();

        if( !current->number ) throw JSONRPCError(RPC_DATABASE_ERROR, "No active reward round available yet.");

        int round = 0;
        std::string err = strprintf("Past SmartReward round required: 1 - %d ",current->number - 1 );

        if (params.size() != 2) throw JSONRPCError(RPC_INVALID_PARAMETER, err);

        try {
             int n = std::stoi(params[1].get_str());
             round = n;
        }
        catch (const std::invalid_argument& ia) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, err);
        }
        catch (const std::out_of_range& oor) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, err);
        }
        catch (...) {}

        if(round < 1 || round >= current->number) throw JSONRPCError(RPC_INVALID_PARAMETER, err);

        CSmartRewardResultEntryList results;

        if( !prewards->GetRewardRoundResults(round, results) )
            throw JSONRPCError(RPC_DATABASE_ERROR, "Couldn't fetch the list from the database.");

        UniValue obj(UniValue::VARR);

        BOOST_FOREACH(CSmartRewardResultEntry s, results) {

            UniValue addrObj(UniValue::VOBJ);
            addrObj.pushKV("address", s.entry.id.ToString());
            addrObj.pushKV("balance", format(s.entry.balance));

            obj.push_back(addrObj);
        }

        return obj;
    }

    if (strCommand == "check")
    {
        if (params.size() != 2) throw JSONRPCError(RPC_INVALID_PARAMETER, "SmartCash address required.");

        TRY_LOCK(cs_rewardscache, cacheLocked);

        if(!cacheLocked) throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");

        const CSmartRewardRound *current = prewards->GetCurrentRound();

        int nFirst_1_3_Round = Params().GetConsensus().nRewardsFirst_1_3_Round;

        std::string addressString = params[1].get_str();
        CSmartAddress id = CSmartAddress::Legacy(addressString);

        if( !id.IsValid() ) throw JSONRPCError(RPC_DATABASE_ERROR, strprintf("Invalid SmartCash address provided: %s",addressString));

        CSmartRewardEntry *entry = nullptr;

        if( !prewards->GetRewardEntry(id, entry, false) ) throw JSONRPCError(RPC_DATABASE_ERROR, "Couldn't find this SmartCash address in the database.");

        UniValue obj(UniValue::VOBJ);

        obj.pushKV("address", id.ToString());
        obj.pushKV("balance", format(entry->balance));
        obj.pushKV("balance_eligible", format(entry->balanceEligible));
        obj.pushKV("is_smartnode", !entry->smartnodePaymentTx.IsNull());
        obj.pushKV("activated", entry->fActivated);
        obj.pushKV("eligible", current->number < nFirst_1_3_Round ? entry->balanceEligible > 0 : entry->IsEligible());

        return obj;
    }

    return NullUniValue;
}

UniValue termrewards(const UniValue& params, bool fHelp)
{
    std::function<double (CAmount)> format = [](CAmount a) {
        return a / COIN + ( double(a % COIN) / COIN );
    };

    if (fHelp) {
        throw std::runtime_error(
            "termrewardds\n"
            "Display addresses currently eligible to TermRewards\n"

            "\nResult (if verbose > 0):\n"
            "[\n"
            " {\n"
            "  \"address\" : \"smartcash address\",  (string) smartcash address\n"
            "  \"tx_hash\" : \"hash\",               (string) hash of the locking tx\n"
            "  \"balance\" : \"term balance\",       (string) Term balance\n"
            "  \"level\" : \"years\",                (string) TermRewards level (1, 2, 3 years)\n"
            "  \"percent\" : \"% Annual Return\",     (string) Return in % per year\n"
            "  \"expires\" : \"expires\",             (string) Term Expiration Date\n"
            " },\n"
            " {\n"
            " ...\n"
            " }\n"
            "]\n"
            );
    }

    if( !fDebug && !prewards->IsSynced() )
        throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is not up to date.");

    TRY_LOCK(cs_rewardsdb, lockRewardsDb);

    if (!lockRewardsDb) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy.  Try again");
    }

    UniValue arr(UniValue::VARR);

    TRY_LOCK(cs_rewardscache, cacheLocked);

    if(!cacheLocked) throw JSONRPCError(RPC_DATABASE_ERROR, "Rewards database is busy..Try it again!");

    CTermRewardEntryMap entries;
    if (!prewards->GetTermRewardsEntries(entries)) throw JSONRPCError(RPC_DATABASE_ERROR, "Failed to get TermRewards entries");

    for (const auto &entry : entries) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("address", entry.second->GetAddress());
        obj.pushKV("tx_hash", entry.second->txHash.GetHex());
        obj.pushKV("balance", format(entry.second->balance));
        obj.pushKV("level", entry.second->GetLevel());
        obj.pushKV("percent", entry.second->percent);
        obj.pushKV("expires", entry.second->expires);
        arr.push_back(obj);
    }

    return arr;
}
