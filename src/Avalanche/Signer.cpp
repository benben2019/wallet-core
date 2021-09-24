// Copyright © 2017-2021 Trust Wallet.
//
// This file is part of Trust. The full Trust copyright notice, including
// terms governing use, modification, and redistribution, is contained in the
// file LICENSE at the root of the source code distribution tree.

#include "Signer.h"
#include "Address.h"
#include "../PublicKey.h"
#include "../Bitcoin/InputSelector.h"
#include "../Bitcoin/FeeCalculator.h"

#include <algorithm>

using namespace TW;
using namespace TW::Avalanche;

std::vector<Address> structToAddresses(
    const google::protobuf::internal::RepeatedStringTypeTraits::RepeatedFieldType inAddrs) {
    std::vector<Address> out;
    for (auto& addrBytes : inAddrs) {
        auto pubkeyData = Data(addrBytes.begin(), addrBytes.end());
        auto pubkey = PublicKey(pubkeyData, TWPublicKeyTypeSECP256k1);
        auto addr = Address(pubkey);
        out.push_back(addr);
    }
    return out;
}

std::vector<Address> keyHashToAddresses(
    const google::protobuf::internal::RepeatedStringTypeTraits::RepeatedFieldType inAddrs) {
    std::vector<Address> out;
    for (auto& addrBytes : inAddrs) {
        auto keyHashData = Data(addrBytes.begin(), addrBytes.end());
        auto addr = Address(keyHashData);
        out.push_back(addr);
    }
    return out;
}

std::vector<TransferableInput> structToInputs(
    const google::protobuf::RepeatedPtrField<Proto::TransferableInput> inputStructs) noexcept {
    std::vector<TransferableInput> inputs;
    for (auto& inputStruct : inputStructs) {
        auto txid = Data(inputStruct.tx_id().begin(), inputStruct.tx_id().end());
        auto utxoIndex = inputStruct.utxo_index();
        auto assetID = Data(inputStruct.asset_id().begin(), inputStruct.asset_id().end());
        std::vector<Address> spendableAddresses =
            structToAddresses(inputStruct.spendable_addresses());

        switch (inputStruct.input().input_case()) {
        case Proto::TransactionInput::kSecpTransferInput: {
            auto secpTxferStruct = inputStruct.input().secp_transfer_input();
            auto amount = secpTxferStruct.amount();
            std::vector<uint32_t> addressIndices;
            for (auto& index : secpTxferStruct.address_indices()) {
                addressIndices.push_back(index);
            }
            auto txnInput = std::make_unique<SECP256k1TransferInput>(amount, addressIndices);
            auto txferInput = TransferableInput(txid, utxoIndex, assetID, std::move(txnInput),
                                                spendableAddresses);
            inputs.push_back(txferInput);
            break;
        }

        case Proto::TransactionInput::INPUT_NOT_SET:
        default: {
            // fail out and return nil inputs if input unset or not supported
            inputs.clear();
            return inputs;
        }
        } // end switch-case deciding which input to build
    }     // end for loop building input structs
    return inputs;
}

std::unique_ptr<TransactionOutput>
extractTransferOut(const Proto::TransactionOutput outputStruct) noexcept {
    switch (outputStruct.output_case()) {
    case Proto::TransactionOutput::kSecpTransferOutput:
    default: {
        auto secpTxferStruct = outputStruct.secp_transfer_output();
        auto amount = secpTxferStruct.amount();
        auto locktime = secpTxferStruct.locktime();
        auto threshold = secpTxferStruct.threshold();
        auto addresses = structToAddresses(secpTxferStruct.addresses());

        return std::make_unique<SECP256k1TransferOutput>(amount, locktime, threshold, addresses);
    }
    } // end switch-case deciding which output struct to build
}

std::vector<TransferableOutput> structToOutputs(
    const google::protobuf::RepeatedPtrField<Proto::TransferableOutput> outputStructs) noexcept {
    std::vector<TransferableOutput> outputs;
    for (auto& outputStruct : outputStructs) {
        auto assetID = Data(outputStruct.asset_id().begin(), outputStruct.asset_id().end());
        auto txnOut = extractTransferOut(outputStruct.output());
        auto txferOut = TransferableOutput(assetID, std::move(txnOut));
        outputs.push_back(txferOut);
    } // end for loop building output structs
    return outputs;
}

BaseTransaction structToBaseTx(const Proto::BaseTx txStruct) noexcept {
    // grab members of struct
    auto typeID = txStruct.type_id();
    auto networkID = txStruct.network_id();
    auto blockchainID = Data(txStruct.blockchain_id().begin(), txStruct.blockchain_id().end());
    auto outputStructs = txStruct.outputs();
    auto inputStructs = txStruct.inputs();
    auto memo = Data(txStruct.memo().begin(), txStruct.memo().end());

    auto outputs = structToOutputs(outputStructs);
    auto inputs = structToInputs(inputStructs);

    return BaseTransaction(typeID, networkID, blockchainID, inputs, outputs, memo);
}

BaseTransaction structToSimpleTransferTx(const Proto::SimpleTransferTx& tx) noexcept {
    assert(tx.has_plan());

    // grab members of struct
    auto typeID = tx.type_id();
    auto networkID = tx.network_id();
    auto blockchainID = Data(tx.blockchain_id().begin(), tx.blockchain_id().end());
    auto memo = Data(tx.memo().begin(), tx.memo().end());

    auto inputStructs = tx.plan().utxos();
    auto inputs = structToInputs(inputStructs);

    // build outputs
    std::vector<TransferableOutput> outputs;
    auto toAddresses = keyHashToAddresses(tx.to_addresses());
    outputs.push_back(TransferableOutput(
        Data(tx.output_asset_id().begin(), tx.output_asset_id().end()),
        std::make_unique<SECP256k1TransferOutput>(tx.plan().amount(), tx.locktime(), tx.threshold(), toAddresses)
    ));
    if (tx.plan().change() > 0) {
        auto changeAddresses = keyHashToAddresses(tx.change_addresses());
        outputs.push_back(TransferableOutput(
            Data(tx.output_asset_id().begin(), tx.output_asset_id().end()),
            std::make_unique<SECP256k1TransferOutput>(tx.plan().change(), tx.locktime(), tx.threshold(), changeAddresses)
        ));
    }

    return BaseTransaction(typeID, networkID, blockchainID, inputs, outputs, memo);
}

BaseTransaction buildBaseTx(const Proto::BaseTx& tx) noexcept {
    return structToBaseTx(tx);
}

BaseTransaction buildSimpleTransferTx(const Proto::SigningInput& input) noexcept {
    auto tx = input.simple_transfer_tx();

    Proto::TransactionPlan plan;
    // plan if needed
    if (!tx.has_plan()) {
        plan = Signer::plan(input);
        *tx.mutable_plan() = plan;
    }

    return structToSimpleTransferTx(tx);
}

Proto::TransactionPlan Signer::plan(const Proto::SigningInput& input) noexcept {
    auto plan = Proto::TransactionPlan();

    if (!input.has_simple_transfer_tx()) {
        // not simple transaction
        return plan;
    }
    auto tx = input.simple_transfer_tx();

    const auto n = tx.inputs_size();
    std::vector<InputProtoWrapper> inputs;
    uint64_t inputSum = 0;
    for (auto i = 0; i < n; ++i) {
        if (tx.inputs(i).input().has_secp_transfer_input()) {
            auto amount = tx.inputs(i).input().secp_transfer_input().amount();
            inputs.push_back(InputProtoWrapper(tx.inputs(i)));
            inputSum += amount;
        }
    }

    bool maxAmount = tx.use_max_amount();
    // if amount requested is the same or more than available amount, it cannot be satisifed, but
    // treat this case as MaxAmount, and send maximum available (which will be less)
    if (!maxAmount && tx.amount() >= inputSum) {
        maxAmount = true;
    }

    auto amount = tx.amount();
    auto fee = tx.fee();

    // Input selection
    auto inputSelector = Bitcoin::InputSelector<InputProtoWrapper>(inputs, Bitcoin::ConstantFeeCalculator(static_cast<int64_t>(fee)));
    std::vector<InputProtoWrapper> selectedInputs;
    if (!maxAmount) {
        selectedInputs = inputSelector.select(amount, 0);
    } else {
        selectedInputs = inputSelector.selectMaxAmount(0);
    }

    uint64_t availAmount = 0;
    for (auto i: selectedInputs) {
        auto utxo = plan.add_utxos();
        *utxo = i._proto;
        availAmount += i.amount();
    }

    fee = std::min(availAmount, tx.fee());
    if (!maxAmount) {
        amount = std::max(uint64_t(0), std::min(amount, availAmount - fee));
    } else {
        // max available amount
        amount = std::max(uint64_t(0), availAmount - fee);
    }
    uint64_t change = (uint64_t)((int64_t)availAmount - (int64_t)(amount + fee));

    plan.set_amount(amount);
    plan.set_available_amount(availAmount);
    plan.set_fee(fee);
    plan.set_change(change);

    return plan;
}

Proto::SigningOutput Signer::sign(const Proto::SigningInput& input) noexcept {
    auto protoOutput = Proto::SigningOutput();

    std::vector<PrivateKey> privateKeys;
    for (auto& privateKeyBytes : input.private_keys()) {
        auto privateKey = PrivateKey(Data(privateKeyBytes.begin(), privateKeyBytes.end()));
        privateKeys.push_back(privateKey);
    }

    Data encoded;
    switch (input.tx_oneof_case()) {
        case Proto::SigningInput::kBaseTx:
            {
                auto transaction = buildBaseTx(input.base_tx());
                encoded = Signer::sign(privateKeys, transaction);
            }
            break;

        case Proto::SigningInput::kSimpleTransferTx:
            {
                auto transaction = buildSimpleTransferTx(input);
                encoded = Signer::sign(privateKeys, transaction);
            }
            break;

        default:
            return protoOutput;
    }
    protoOutput.set_encoded(encoded.data(), encoded.size());

    return protoOutput;
}

Data Signer::sign(const std::vector<PrivateKey>& privateKeys,
                  BaseTransaction& transaction) noexcept {
    // see avalanchejs/src/apis/avm/basetx.ts and tx.ts for reference implementations
    const Data codecID = {0x0, 0x0};
    Data transactionBytes;
    transactionBytes.insert(std::end(transactionBytes), std::begin(codecID), std::end(codecID));
    transaction.encode(transactionBytes);
    auto msgBytes = Hash::sha256(transactionBytes);
    std::vector<Credential> credentials;
    for (auto& input : transaction.inputs) {
        if (input.input->getTypeID() == TransactionInputTypeID::SECPInput) {
            // secp input, make an SECP credential
            std::vector<Data> sigs;
            for (auto& sigidx : input.input->getAddressIndices()) {
                auto addresses = input.spendableAddresses;
                std::sort(addresses.begin(), addresses.end());
                if (sigidx >= addresses.size()) {
                    // would cause a crash, sigidx does not exist in the address vector.
                    // return empty Data.
                    return Data{};
                }
                auto addressRequested = addresses[sigidx];
                for (auto& key : privateKeys) {
                    auto possibleAddress = Address(key.getPublicKey(TWPublicKeyTypeSECP256k1));
                    if (possibleAddress == addressRequested) {
                        auto signature = key.sign(msgBytes, TWCurveSECP256k1);
                        sigs.push_back(signature);
                    }
                }
            }
            auto credential = SECP256k1Credential(sigs);
            credentials.push_back(credential);
        } else {
            // nft input, make an NFT credential
            // NFT inputs not currently supported. Return empty Data.
            return Data{};
        }
    }
    auto signedTransaction = SignedTransaction(transaction, credentials);
    Data outputData;
    signedTransaction.encode(outputData);
    return outputData;
}
