// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 Conceal Network & Conceal Devs
// Copyright (c) 2019-2020 The Lithe Project Development Team

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "SimpleWallet.h"

#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <thread>
#include <set>
#include <sstream>
#include <regex>

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "Common/Base58.h"
#include "Common/CommandLine.h"
#include "Common/ColouredMsg.h"
#include "Common/SignalHandler.h"
#include "Common/StringTools.h"
#include "Common/PathTools.h"
#include "Common/Util.h"
#include "Common/DnsTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "Rpc/HttpClient.h"
#include "CryptoNoteCore/CryptoNoteTools.h"

#include "Wallet/WalletRpcServer.h"
#include "WalletLegacy/WalletLegacy.h"
#include "Wallet/LegacyKeysImporter.h"
#include "WalletLegacy/WalletHelper.h"
#include "Mnemonics/electrum-words.cpp"

#include "version.h"

#include <Logging/LoggerManager.h>

#include <tabulate/table.hpp>
using namespace tabulate;

#if defined(WIN32)
#include <crtdbg.h>
#endif

using namespace CryptoNote;
using namespace Logging;
using Common::JsonValue;

namespace po = boost::program_options;

#define EXTENDED_LOGS_FILE "wallet_details.log"
#undef ERROR

namespace {

const command_line::arg_descriptor<std::string> arg_wallet_file = { "wallet-file", "Use wallet <arg>", "" };
const command_line::arg_descriptor<std::string> arg_generate_new_wallet = { "generate-new-wallet", "Generate new wallet and save it to <arg>", "" };
const command_line::arg_descriptor<std::string> arg_daemon_address = { "daemon-address", "Use daemon instance at <host>:<port>", "" };
const command_line::arg_descriptor<std::string> arg_daemon_host = { "daemon-host", "Use daemon instance at host <arg> instead of localhost", "" };
const command_line::arg_descriptor<std::string> arg_password = { "password", "Wallet password", "", true };
const command_line::arg_descriptor<uint16_t>    arg_daemon_port = { "daemon-port", "Use daemon instance at port <arg> instead of default", 0 };
const command_line::arg_descriptor<uint32_t>    arg_log_level = { "set_log", "", INFO, true };
const command_line::arg_descriptor<bool>        arg_testnet = { "testnet", "Used to deploy test nets. The daemon must be launched with --testnet flag", false };
const command_line::arg_descriptor<bool>        arg_sync_from_zero = {"sync_from_zero", "Sync from block 0. Use for premine wallet", false};
const command_line::arg_descriptor<bool>        arg_exit_after_generate = {"exit-after-generate", "Exit immediately after generating a wallet (doesn't try to sync with the daemon)", false};
const command_line::arg_descriptor<std::vector<std::string>> arg_command = { "command", "" };

bool parseUrlAddress(const std::string& url, std::string& address, uint16_t& port) {
  auto pos = url.find("://");
  uint64_t addrStart = 0;

  if (pos != std::string::npos) {
    addrStart = pos + 3;
  }

  auto addrEnd = url.find(':', addrStart);

  if (addrEnd != std::string::npos) {
    auto portEnd = url.find('/', addrEnd);
    port = Common::fromString<uint16_t>(url.substr(
      addrEnd + 1, portEnd == std::string::npos ? std::string::npos : portEnd - addrEnd - 1));
  } else {
    addrEnd = url.find('/');
    port = 80;
  }

  address = url.substr(addrStart, addrEnd - addrStart);
  return true;
}


inline std::string interpret_rpc_response(bool ok, const std::string& status) {
  std::string err;
  if (ok) {
    if (status == CORE_RPC_STATUS_BUSY) {
      err = "daemon is busy. Please try later";
    } else if (status != CORE_RPC_STATUS_OK) {
      err = status;
    }
  } else {
    err = "possible lost connection to daemon";
  }
  return err;
}

JsonValue buildLoggerConfiguration(Level level, const std::string& logfile) {
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue& cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue& consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "");

  JsonValue& fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  return loggerConfiguration;
}

std::error_code initAndLoadWallet(IWalletLegacy& wallet, std::istream& walletFile, const std::string& password) {
  WalletHelper::InitWalletResultObserver initObserver;
  std::future<std::error_code> f_initError = initObserver.initResult.get_future();

  WalletHelper::IWalletRemoveObserverGuard removeGuard(wallet, initObserver);
  wallet.initAndLoad(walletFile, password);
  auto initError = f_initError.get();

  return initError;
}

std::string tryToOpenWalletOrLoadKeysOrThrow(LoggerRef& logger, std::unique_ptr<IWalletLegacy>& wallet, const std::string& walletFile, const std::string& password) {
  std::string keys_file, walletFileName;
  WalletHelper::prepareFileNames(walletFile, keys_file, walletFileName);

  boost::system::error_code ignore;
  bool keysExists = boost::filesystem::exists(keys_file, ignore);
  bool walletExists = boost::filesystem::exists(walletFileName, ignore);
  if (!walletExists && !keysExists && boost::filesystem::exists(walletFile, ignore)) {
    boost::system::error_code renameEc;
    boost::filesystem::rename(walletFile, walletFileName, renameEc);
    if (renameEc) {
      throw std::runtime_error("failed to rename file '" + walletFile + "' to '" + walletFileName + "': " + renameEc.message());
    }

    walletExists = true;
  }

  if (walletExists) {
    std::cout << GreenMsg("Loading Wallet...") << std::endl;
    std::ifstream walletFile;
    walletFile.open(walletFileName, std::ios_base::binary | std::ios_base::in);
    if (walletFile.fail()) {
      throw std::runtime_error("error opening wallet file '" + walletFileName + "'");
    }

    auto initError = initAndLoadWallet(*wallet, walletFile, password);

    walletFile.close();
    if (initError) { //bad password, or legacy format
      if (keysExists) {
        std::stringstream ss;
        CryptoNote::importLegacyKeys(keys_file, password, ss);
        boost::filesystem::rename(keys_file, keys_file + ".back");
        boost::filesystem::rename(walletFileName, walletFileName + ".back");

        initError = initAndLoadWallet(*wallet, ss, password);
        if (initError) {
          throw std::runtime_error("failed to load wallet: " + initError.message());
        }

        std::cout << GreenMsg("Storing Wallet...") << std::endl;

        try {
          CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
        } catch (std::exception& e) {
          logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
          throw std::runtime_error("error saving wallet file '" + walletFileName + "'");
        }

        std::cout << BrightGreenMsg("Successfully stored.") << std::endl;
        return walletFileName;
      } else { // no keys, wallet error loading
        throw std::runtime_error("can't load wallet file '" + walletFileName + "', check password");
      }
    } else { //new wallet ok
      return walletFileName;
    }
  } else if (keysExists) { //wallet not exists but keys presented
    std::stringstream ss;
    CryptoNote::importLegacyKeys(keys_file, password, ss);
    boost::filesystem::rename(keys_file, keys_file + ".back");

    WalletHelper::InitWalletResultObserver initObserver;
    std::future<std::error_code> f_initError = initObserver.initResult.get_future();

    WalletHelper::IWalletRemoveObserverGuard removeGuard(*wallet, initObserver);
    wallet->initAndLoad(ss, password);
    auto initError = f_initError.get();

    removeGuard.removeObserver();
    if (initError) {
      throw std::runtime_error("failed to load wallet: " + initError.message());
    }

    std::cout << GreenMsg("Storing Wallet...") << std::endl;

    try {
      CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);
    } catch(std::exception& e) {
      logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
      throw std::runtime_error("error saving wallet file '" + walletFileName + "'");
    }

    std::cout << BrightGreenMsg("Successfully stored.") << std::endl;
    return walletFileName;
  } else { //no wallet no keys
    throw std::runtime_error("wallet file '" + walletFileName + "' is not found");
  }
}

std::string makeCenteredString(uint64_t width, const std::string& text) {
  if (text.size() >= width) {
    return text;
  }

  uint64_t offset = (width - text.size() + 1) / 2;
  return std::string(offset, ' ') + text + std::string(width - text.size() - offset, ' ');
}

const uint64_t TIMESTAMP_MAX_WIDTH = 19;
const uint64_t HASH_MAX_WIDTH = 64;
const uint64_t TOTAL_AMOUNT_MAX_WIDTH = 20;
const uint64_t FEE_MAX_WIDTH = 14;
const uint64_t BLOCK_MAX_WIDTH = 7;
const uint64_t UNLOCK_TIME_MAX_WIDTH = 11;

void printListTransfersHeader(LoggerRef& logger) {
  std::string header = makeCenteredString(TIMESTAMP_MAX_WIDTH, "timestamp (UTC)") + "  ";
  header += makeCenteredString(HASH_MAX_WIDTH, "hash") + "  ";
  header += makeCenteredString(TOTAL_AMOUNT_MAX_WIDTH, "total amount") + "  ";
  header += makeCenteredString(FEE_MAX_WIDTH, "fee") + "  ";
  header += makeCenteredString(BLOCK_MAX_WIDTH, "block") + "  ";
  header += makeCenteredString(UNLOCK_TIME_MAX_WIDTH, "unlock time");

  std::cout << BrightMagentaMsg(header) << std::endl;
  std::cout << BrightMagentaMsg(std::string(header.size(), '-')) << std::endl;
}

void printListTransfersItem(LoggerRef& logger, const WalletLegacyTransaction& txInfo, IWalletLegacy& wallet, const Currency& currency) {
  std::vector<uint8_t> extraVec = Common::asBinaryArray(txInfo.extra);

  Crypto::Hash paymentId;
  std::string paymentIdStr = (getPaymentIdFromTxExtra(extraVec, paymentId) && paymentId != NULL_HASH ? Common::podToHex(paymentId) : "");

  char timeString[TIMESTAMP_MAX_WIDTH + 1];
  time_t timestamp = static_cast<time_t>(txInfo.timestamp);
  if (std::strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", std::gmtime(&timestamp)) == 0) {
    throw std::runtime_error("time buffer is too small");
  }

  if (txInfo.totalAmount < 0) {
    std::cout << std::setw(TIMESTAMP_MAX_WIDTH) << BrightMagentaMsg(timeString) << std::endl
              << "  " << std::setw(HASH_MAX_WIDTH) << YellowMsg(Common::podToHex(txInfo.hash)) << std::endl
              << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << YellowMsg(currency.formatAmount(txInfo.totalAmount)) << std::endl
              << "  " << std::setw(FEE_MAX_WIDTH) << YellowMsg(currency.formatAmount(txInfo.fee)) << std::endl
              << "  " << std::setw(BLOCK_MAX_WIDTH) << YellowMsg(std::to_string(txInfo.blockHeight)) << std::endl
              << "  " << std::setw(UNLOCK_TIME_MAX_WIDTH) << YellowMsg(std::to_string(txInfo.unlockTime)) << std::endl;
    
    if (!paymentIdStr.empty()) {
      std::cout << YellowMsg("Payment ID: ") << YellowMsg(paymentIdStr) << std::endl;
    }

    if (txInfo.transferCount > 0) {
      std::cout << YellowMsg("Transfers:") << std::endl;
      for (TransferId id = txInfo.firstTransferId; id < txInfo.firstTransferId + txInfo.transferCount; ++id) {
        WalletLegacyTransfer tr;
        wallet.getTransfer(id, tr);
        std::cout << YellowMsg(tr.address) << YellowMsg("  ") << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << YellowMsg(currency.formatAmount(tr.amount)) << std::endl;
      }
    }
  } else if (txInfo.totalAmount > 0) {
    std::cout << std::setw(TIMESTAMP_MAX_WIDTH) << BrightMagentaMsg(timeString) << std::endl
              << "  " << std::setw(HASH_MAX_WIDTH) << GreenMsg(Common::podToHex(txInfo.hash)) << std::endl
              << "  " << std::setw(TOTAL_AMOUNT_MAX_WIDTH) << GreenMsg(currency.formatAmount(txInfo.totalAmount)) << std::endl
              << "  " << std::setw(FEE_MAX_WIDTH) << GreenMsg(currency.formatAmount(txInfo.fee)) << std::endl
              << "  " << std::setw(BLOCK_MAX_WIDTH) << GreenMsg(std::to_string(txInfo.blockHeight)) << std::endl
              << "  " << std::setw(UNLOCK_TIME_MAX_WIDTH) << GreenMsg(std::to_string(txInfo.unlockTime)) << std::endl;
    
    if (!paymentIdStr.empty()) {
      std::cout << GreenMsg("Payment ID: ") << GreenMsg(paymentIdStr) << std::endl;
    }
  }

  std::cout << std::endl; /* just to make logger print one endline */
}

std::string prepareWalletAddressFilename(const std::string& walletBaseName) {
  return walletBaseName + ".address";
}

bool writeAddressFile(const std::string& addressFilename, const std::string& address) {
  std::ofstream addressFile(addressFilename, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!addressFile.good()) {
    return false;
  }

  addressFile << address;

  return true;
}

bool processServerAliasResponse(const std::string& s, std::string& address) {
  try {
  //   
  // Courtesy of Monero Project
		// make sure the txt record has "oa1:lxth" and find it
		auto pos = s.find("oa1:lxth");
		if (pos == std::string::npos)
			return false;
		// search from there to find "recipient_address="
		pos = s.find("recipient_address=", pos);
		if (pos == std::string::npos)
			return false;
		pos += 18; // move past "recipient_address="
		// find the next semicolon
		auto pos2 = s.find(";", pos);
		if (pos2 != std::string::npos)
		{
			// length of address == 95, we can at least validate that much here
			if (pos2 - pos == 98)
			{
				address = s.substr(pos, 98);
			} else {
				return false;
			}
		}
    }
	catch (std::exception&) {
		return false;
	}

	return true;
}

bool askAliasesTransfersConfirmation(const std::map<std::string, std::vector<WalletLegacyTransfer>>& aliases, const Currency& currency) {
  std::cout << "Would you like to send money to the following addresses?" << std::endl;

  for (const auto& kv: aliases) {
    for (const auto& transfer: kv.second) {
      std::cout << transfer.address << " " << std::setw(21) << currency.formatAmount(transfer.amount) << "  " << kv.first << std::endl;
    }
  }

  std::string answer;
  do {
    std::cout << "y/n: ";
    std::getline(std::cin, answer);
  } while (answer != "y" && answer != "Y" && answer != "n" && answer != "N");

  return answer == "y" || answer == "Y";
}

bool processServerFeeAddressResponse(const std::string& response, std::string& fee_address) {
    try {
        std::stringstream stream(response);
        JsonValue json;
        stream >> json;

        auto rootIt = json.getObject().find("fee_address");
        if (rootIt == json.getObject().end()) {
            return false;
        }

        fee_address = rootIt->second.getString();
    }
    catch (std::exception&) {
        return false;
    }

    return true;
}

} // namespace


std::string simple_wallet::get_commands_str() {
  std::stringstream ss;
  ss << "Basic Commands: " << ENDL;
  std::string usage = m_consoleHandler.getUsage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}

std::string simple_wallet::get_adv_commands_str() {
  std::stringstream ss;
  ss << "Advanced Commands: " << ENDL;
  std::string usage = m_consoleHandler.getUsageAdv();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}

bool simple_wallet::help(const std::vector<std::string> &args) {
  std::cout << get_commands_str() << std::endl;
  return true;
}

bool simple_wallet::advanced(const std::vector<std::string> &args) {
  std::cout << get_adv_commands_str() << std::endl;
  return true;
}

bool simple_wallet::exit(const std::vector<std::string> &args) {
  m_consoleHandler.requestStop();
  return true;
}

simple_wallet::simple_wallet(System::Dispatcher& dispatcher, const CryptoNote::Currency& currency, Logging::LoggerManager& log) :
  m_dispatcher(dispatcher),
  m_daemon_port(0),
  m_currency(currency),
  logManager(log),
  logger(log, "simplewallet"),
  m_refresh_progress_reporter(*this),
  m_initResultPromise(nullptr),
  m_walletSynchronized(false) {
  m_consoleHandler.setHandler("balance", boost::bind(&simple_wallet::show_balance, this, _1), "Show current wallet balance");
  m_consoleHandler.setHandler("incoming_transfers", boost::bind(&simple_wallet::show_incoming_transfers, this, _1), "Show incoming transfers");
  m_consoleHandler.setHandler("outgoing_transfers", boost::bind(&simple_wallet::show_outgoing_transfers, this, _1), "Show outgoing transfers");
  m_consoleHandler.setHandler("list_transfers", boost::bind(&simple_wallet::listTransfers, this, _1), "list_transfers <height> - Show all known transfers from a certain (optional) block height");
  m_consoleHandler.setHandler("wallet_info", boost::bind(&simple_wallet::show_wallet_info, this, _1), "Show blockchain height");
  m_consoleHandler.setHandler("transfer", boost::bind(&simple_wallet::transfer, this, _1),
    "transfer <addr_1> <amount_1> [<addr_2> <amount_2> ... <addr_N> <amount_N>] [-p payment_id]"
    " - Transfer <amount_1>,... <amount_N> to <address_1>,... <address_N>, respectively. ");
  m_consoleHandler.setHandler("address", boost::bind(&simple_wallet::print_address, this, _1), "Show current wallet public address");
  m_consoleHandler.setHandler("save", boost::bind(&simple_wallet::save, this, _1), "Save wallet synchronized data");
  m_consoleHandler.setHandler("reset", boost::bind(&simple_wallet::reset, this, _1), "Discard cache data and start synchronizing from the start");
  m_consoleHandler.setHandler("help", boost::bind(&simple_wallet::help, this, _1), "Show the Basic commands menu.");
  m_consoleHandler.setHandler("exit", boost::bind(&simple_wallet::exit, this, _1), "Close wallet");
  m_consoleHandler.setHandler("advanced", boost::bind(&simple_wallet::advanced, this, _1), "Shows the Advanced commands menu.");
  m_consoleHandler.setHandlerAdv("optimize", boost::bind(&simple_wallet::optimize_outputs, this, _1), "Combine many available outputs into a few by sending a transaction to self");
  m_consoleHandler.setHandlerAdv("optimize_all", boost::bind(&simple_wallet::optimize_all_outputs, this, _1), "Optimize your wallet several times so you can send large transactions");
  m_consoleHandler.setHandlerAdv("set_log", boost::bind(&simple_wallet::set_log, this, _1), "set_log <level> - Change current log level, <level> is a number 0-4");
  m_consoleHandler.setHandlerAdv("outputs", boost::bind(&simple_wallet::show_num_unlocked_outputs, this, _1), "Show the number of unlocked outputs available for a transaction"); 
  m_consoleHandler.setHandlerAdv("payments", boost::bind(&simple_wallet::show_payments, this, _1), "payments <payment_id_1> [<payment_id_2> ... <payment_id_N>] - Show payments <payment_id_1>, ... <payment_id_N>");
  m_consoleHandler.setHandlerAdv("create_integrated", boost::bind(&simple_wallet::create_integrated, this, _1), "create_integrated <payment_id> - Create an integrated address with a payment ID");
  m_consoleHandler.setHandlerAdv("export_keys", boost::bind(&simple_wallet::export_keys, this, _1), "Show the secret keys of the current wallet");
  m_consoleHandler.setHandlerAdv("sign_message", boost::bind(&simple_wallet::sign_message, this, _1), "Sign a message with your wallet keys");
  m_consoleHandler.setHandlerAdv("verify_signature", boost::bind(&simple_wallet::verify_signature, this, _1), "Verify a signed message");
  m_consoleHandler.setHandlerAdv("show_dust", boost::bind(&simple_wallet::show_dust, this, _1), "Show the number of unmixable dust outputs");
}

/* This function shows the number of outputs in the wallet
  that are below the dust threshold */
bool simple_wallet::show_dust(const std::vector<std::string>& args) {
  std::cout << YellowMsg("Dust outputs: ") << YellowMsg(std::to_string(m_wallet->dustBalance())) << std::endl;
	return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::set_log(const std::vector<std::string> &args) {
  if (args.size() != 1) {
    std::cout << RedMsg("Use: set_log <0-4>") << std::endl;
    return true;
  }

  uint16_t l = 0;
  if (!Common::fromString(args[0], l)) {
    std::cout << RedMsg("Wrong number format. Use: set_log <0-4>") << std::endl;
    return true;
  }

  if (l > Logging::TRACE) {
    std::cout << RedMsg("Wrong number range. Use: set_log <0-4>") << std::endl;
    return true;
  }

  logManager.setMaxLevel(static_cast<Logging::Level>(l));
  return true;
}

bool key_import = true;

//----------------------------------------------------------------------------------------------------
bool simple_wallet::init(const boost::program_options::variables_map& vm) {
  handle_command_line(vm);

  if (!m_daemon_address.empty() && (!m_daemon_host.empty() || 0 != m_daemon_port)) {
    logger(DEBUGGING) << "User tried to specify Daemon host or port several times.";
    std::cout << RedMsg("You can't specify Daemon host or port several times. Please choose only one.") << std::endl;
    return false;
  }

  if (m_daemon_host.empty())
    m_daemon_host = "localhost";
  if (!m_daemon_port)
    m_daemon_port = RPC_DEFAULT_PORT;

  if (!m_daemon_address.empty()) {
    if (!parseUrlAddress(m_daemon_address, m_daemon_host, m_daemon_port)) {
      logger(DEBUGGING) << "Failed to parse Daemon address: " << m_daemon_address;
      std::cout << RedMsg("Failed to parse Daemon address: ") << YellowMsg(m_daemon_address) << std::endl;
      return false;
    }

    remote_fee_address = getFeeAddress();
    std::cout << BrightGreenMsg("Connected to Remote Node: ") << BrightMagentaMsg(m_daemon_host) << std::endl;
    if (!remote_fee_address.empty()) {
      std::cout << GreenMsg("Fee Address: ") << MagentaMsg(remote_fee_address) << std::endl;
    }    
  } else {
    if (!m_daemon_host.empty()) 
      remote_fee_address = getFeeAddress();
		m_daemon_address = std::string("http://") + m_daemon_host + ":" + std::to_string(m_daemon_port);
    std::cout << BrightGreenMsg("Connected to Remote Node: ") << BrightMagentaMsg(m_daemon_host) << std::endl;
    
    if (!remote_fee_address.empty()) {
      std::cout << GreenMsg("Fee Address: ") << MagentaMsg(remote_fee_address) << std::endl;
    }   
  }

  if (m_generate_new.empty() && m_wallet_file_arg.empty()) {

    std::cout << std::endl << "Welcome, please choose an option below:"<< std::endl 
              << std::endl << BrightMagentaMsg("\t[G]") << " - Generate a new wallet address"
              << std::endl << BrightMagentaMsg("\t[O]") << " - Open a wallet already on your system"
              << std::endl << BrightMagentaMsg("\t[S]") << " - Regenerate your wallet using a seed phrase of words"
              << std::endl << BrightMagentaMsg("\t[I]") << " - Import your wallet using a View Key and Spend Key"<< std::endl 
              << std::endl << YellowMsg("or, press CTRL_C to exit: ") << std::flush;

    char c;
    do {
      std::string answer;
      std::getline(std::cin, answer);
      c = answer[0];
      c = std::tolower(c);
      if (!(c == 'o' || c == 'g' || c == 'i' || c == 's')) {
        std::cout << "Unknown command: " << answer << std::endl;
      } else {
        break;
      }
    } while (true);

    if (c == 'e') {
      return false;
    }

    std::cout << BrightGreenMsg("Specify wallet file name ")
              << BrightMagentaMsg("(e.g., name.wallet).\n") << std::endl;
    std::string userInput;
    do {
      if (c == 'o') {
        std::cout << BrightGreenMsg("Enter the name of the wallet you wish to open: ");
      } else {
        std::cout << BrightGreenMsg("What do you want to call your new wallet?: ");
      }
      std::getline(std::cin, userInput);
      boost::algorithm::trim(userInput);
    } while (userInput.empty());
    if (c == 'i') {
      key_import = true;
      m_import_new = userInput;
    } else if (c == 's') {
      key_import = false;
      m_import_new = userInput;
    } else if (c == 'g') {
      m_generate_new = userInput;
    } else {
      m_wallet_file_arg = userInput;
    }
  }

  if (!m_generate_new.empty() && !m_wallet_file_arg.empty() && !m_import_new.empty()) {
    logger(DEBUGGING) << "User tried to use generate-new-wallet and wallet-file together.";
    std::cout << RedMsg("You can't specify the \"generate-new-wallet\" and \"wallet-file\" arguments simultaneously.") << std::endl;
    return false;
  }

  std::string walletFileName;
  m_sync_from_zero = command_line::get_arg(vm, arg_sync_from_zero);
  if (m_sync_from_zero) {
    sync_from_height = 0;
  }

  if (!m_generate_new.empty() || !m_import_new.empty()) {
    std::string ignoredString;
    if (!m_generate_new.empty()) {
      WalletHelper::prepareFileNames(m_generate_new, ignoredString, walletFileName);
    } else if (!m_import_new.empty()) {
      WalletHelper::prepareFileNames(m_import_new, ignoredString, walletFileName);
    }
    boost::system::error_code ignore;
    if (boost::filesystem::exists(walletFileName, ignore)) {
      logger(DEBUGGING) << "User tried to create a wallet with a filename that already exists.";
      std::cout << YellowMsg(walletFileName) << RedMsg(" already exists. Please choose a new name") << std::endl;
      return false;
    }
  }

  if (command_line::has_arg(vm, arg_password)) {
    m_pwd_container.password(command_line::get_arg(vm, arg_password));
  } else if (!m_pwd_container.read_password(!m_generate_new.empty() || !m_import_new.empty())) {
    logger(DEBUGGING) << "Failed to read Wallet password.";
    std::cout << RedMsg("Failed to read Wallet password.") << std::endl;
    return false;
  }

  this->m_node.reset(new NodeRpcProxy(m_daemon_host, m_daemon_port));

  std::promise<std::error_code> errorPromise;
  std::future<std::error_code> f_error = errorPromise.get_future();
  auto callback = [&errorPromise](std::error_code e) {errorPromise.set_value(e); };

  m_node->addObserver(static_cast<INodeRpcProxyObserver*>(this));
  m_node->init(callback);
  auto error = f_error.get();
  if (error) {
    logger(DEBUGGING) << "Failed to init NodeRPCProxy: " << error.message();
    std::cout << RedMsg("Failed to init NodeRPCProxy: ") << YellowMsg(error.message()) << std::endl;
    return false;
  }

  m_sync_from_zero = command_line::get_arg(vm, arg_sync_from_zero);
  if (m_sync_from_zero) {
    sync_from_height = 0;
  }

  if (!m_generate_new.empty()) {
    std::string walletAddressFile = prepareWalletAddressFilename(m_generate_new);
    boost::system::error_code ignore;
    if (boost::filesystem::exists(walletAddressFile, ignore)) {
      logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
      return false;
    }

    if (!new_wallet(walletFileName, m_pwd_container.password())) {
      logger(ERROR, BRIGHT_RED) << "account creation failed";
      return false;
    }

    if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
      logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
    }
  } else if (!m_import_new.empty()) {
    std::string walletAddressFile = prepareWalletAddressFilename(m_import_new);
    boost::system::error_code ignore;
    if (boost::filesystem::exists(walletAddressFile, ignore)) {
      logger(ERROR, BRIGHT_RED) << "Address file already exists: " + walletAddressFile;
      return false;
    }

    std::string private_spend_key_string;
    std::string private_view_key_string;
    Crypto::SecretKey private_spend_key;
    Crypto::SecretKey private_view_key;
    
    if (key_import) {
      do {
        std::cout << "Private Spend Key: ";
        std::getline(std::cin, private_spend_key_string);
        boost::algorithm::trim(private_spend_key_string);
      } while (private_spend_key_string.empty());
      do {
        std::cout << "Private View Key: ";
        std::getline(std::cin, private_view_key_string);
        boost::algorithm::trim(private_view_key_string);
      } while (private_view_key_string.empty());
    } else {
      std::string mnemonic_phrase;
      do {
        std::cout << "Mnemonics Phrase (25 words): ";
        std::getline(std::cin, mnemonic_phrase);
        boost::algorithm::trim(mnemonic_phrase);
        boost::algorithm::to_lower(mnemonic_phrase);
      } while (!is_valid_mnemonic(mnemonic_phrase, private_spend_key));
      /* This is not used, but is needed to be passed to the function, not sure how we can avoid this */
      Crypto::PublicKey unused_dummy_variable;
      AccountBase::generateViewFromSpend(private_spend_key, private_view_key, unused_dummy_variable);
    }
    
    /* We already have our keys if we import via mnemonic seed */
    if (key_import) {
      Crypto::Hash private_spend_key_hash;
      Crypto::Hash private_view_key_hash;
      uint64_t size;
      if (!Common::fromHex(private_spend_key_string, &private_spend_key_hash, sizeof(private_spend_key_hash), size) || size != sizeof(private_spend_key_hash)) {
        return false;
      }
      if (!Common::fromHex(private_view_key_string, &private_view_key_hash, sizeof(private_view_key_hash), size) || size != sizeof(private_spend_key_hash)) {
        return false;
      }
      private_spend_key = *(struct Crypto::SecretKey *) &private_spend_key_hash;
      private_view_key = *(struct Crypto::SecretKey *) &private_view_key_hash;
    }

    if (!new_wallet(private_spend_key, private_view_key, walletFileName, m_pwd_container.password())) {
      logger(ERROR, BRIGHT_RED) << "account creation failed";
      return false;
    }

    if (!writeAddressFile(walletAddressFile, m_wallet->getAddress())) {
      logger(WARNING, BRIGHT_RED) << "Couldn't write wallet address file: " + walletAddressFile;
    }
  } else {
    if(!m_exit_after_generate) {
      m_wallet.reset(new WalletLegacy(m_currency, *m_node, logManager));
      m_wallet->syncAll(m_sync_from_zero, 0);
    }

    try {
      m_wallet_file = tryToOpenWalletOrLoadKeysOrThrow(logger, m_wallet, m_wallet_file_arg, m_pwd_container.password());
    } catch (const std::exception& e) {
      logger(DEBUGGING) << "Failed to load wallet: " << e.what();
      std::cout << RedMsg("Failed to load wallet: ") << YellowMsg(e.what()) << std::endl;
      return false;
    }

    m_wallet->addObserver(this);
    m_node->addObserver(static_cast<INodeObserver*>(this));

    std::cout << BrightGreenMsg("Opened Wallet: ") << BrightMagentaMsg(m_wallet->getAddress()) << std::endl << std::endl;
    std::cout << YellowMsg("Use \"help\" command to see the list of available commands.\n") << std::endl;
    
    if(m_exit_after_generate) {
      m_consoleHandler.requestStop();
      std::exit(0);
    }
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
/* adding support for 25 word electrum seeds. however, we have to ensure that all old wallets that are
not deterministic, dont get a seed to avoid any loss of funds.
*/
std::string simple_wallet::generate_mnemonic(Crypto::SecretKey &private_spend_key) {

  std::string mnemonic_str;

  if (!crypto::ElectrumWords::bytes_to_words(private_spend_key, mnemonic_str, "English")) {
      logger(ERROR, BRIGHT_RED) << "Cant create the mnemonic for the private spend key!";
  }

  return mnemonic_str;
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::log_incorrect_words(std::vector<std::string> words) {
  Language::Base *language = Language::Singleton<Language::English>::instance();
  const std::vector<std::string> &dictionary = language->get_word_list();

  for (auto i : words) {
    if (std::find(dictionary.begin(), dictionary.end(), i) == dictionary.end()) {
      logger(ERROR, BRIGHT_RED) << i << " is not in the english word list!";
    }
  }
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::is_valid_mnemonic(std::string &mnemonic_phrase, Crypto::SecretKey &private_spend_key) {

  static std::string languages[] = {"English"};
  static const int num_of_languages = 1;
  static const int mnemonic_phrase_length = 25;

  std::vector<std::string> words;

  words = boost::split(words, mnemonic_phrase, ::isspace);

  if (words.size() != mnemonic_phrase_length) {
    logger(ERROR, BRIGHT_RED) << "Invalid mnemonic phrase!";
    logger(ERROR, BRIGHT_RED) << "Seed phrase is not 25 words! Please try again.";
    log_incorrect_words(words);
    return false;
  }

  for (int i = 0; i < num_of_languages; i++) {
    if (crypto::ElectrumWords::words_to_bytes(mnemonic_phrase, private_spend_key, languages[i])) {
      return true;
    }
  }

  logger(ERROR, BRIGHT_RED) << "Invalid mnemonic phrase!";
  log_incorrect_words(words);
  return false;
}
//----------------------------------------------------------------------------------------------------


//----------------------------------------------------------------------------------------------------
bool simple_wallet::deinit() {
  m_wallet->removeObserver(this);
  m_node->removeObserver(static_cast<INodeObserver*>(this));
  m_node->removeObserver(static_cast<INodeRpcProxyObserver*>(this));

  if (!m_wallet.get())
    return true;

  return close_wallet();
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::handle_command_line(const boost::program_options::variables_map& vm) {
  m_wallet_file_arg = command_line::get_arg(vm, arg_wallet_file);
  m_generate_new = command_line::get_arg(vm, arg_generate_new_wallet);
  m_daemon_address = command_line::get_arg(vm, arg_daemon_address);
  m_daemon_host = command_line::get_arg(vm, arg_daemon_host);
  m_daemon_port = command_line::get_arg(vm, arg_daemon_port);
  m_exit_after_generate = command_line::get_arg(vm, arg_exit_after_generate);
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_wallet(const std::string &wallet_file, const std::string& password) {
  m_wallet_file = wallet_file;

  m_wallet.reset(new WalletLegacy(m_currency, *m_node, logManager));
  m_node->addObserver(static_cast<INodeObserver*>(this));
  m_wallet->addObserver(this);
  try {
    m_initResultPromise.reset(new std::promise<std::error_code>());
    std::future<std::error_code> f_initError = m_initResultPromise->get_future();
    m_wallet->syncAll(m_sync_from_zero, 0);
    m_wallet->initAndGenerate(password);
    auto initError = f_initError.get();
    m_initResultPromise.reset(nullptr);
    if (initError) {
      logger(DEBUGGING) << "Failed to generate a new wallet: " << initError.message();
      std::cout << RedMsg("Failed to generate a new wallet: ") << YellowMsg(initError.message()) << std::endl;
      return false;
    }

    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (std::exception& e) {
      logger(DEBUGGING) << "Failed to save new wallet: " << e.what();
      std::cout << RedMsg("Failed to save new wallet: ") << YellowMsg(e.what()) << std::endl;
      throw;
    }

    AccountKeys keys;
    m_wallet->getAccountKeys(keys);

    std::string secretKeysData = std::string(reinterpret_cast<char*>(&keys.spendSecretKey), sizeof(keys.spendSecretKey)) + std::string(reinterpret_cast<char*>(&keys.viewSecretKey), sizeof(keys.viewSecretKey));
    std::string guiKeys = Tools::Base58::encode_addr(CryptoNote::parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX, secretKeysData);

    std::cout << "" << std::endl
              << BrightMagentaMsg("lithe-wallet is an open-source, client-side, free wallet which") << std::endl
              << BrightMagentaMsg("allow you to send and receive $LXTH instantly on the blockchain.") << std::endl
              << "" << std::endl
              << "You are in control of your funds & your keys." << std::endl
              << "" << std::endl
              << "When you generate a new wallet, login, send, receive or deposit $LXTH - everything happens locally." << std::endl
              << "" << std::endl
              << "Your seed is never transmitted, received or stored - anywhere." << std::endl
              << "That's why its imperative to write, print or save your seed somewhere safe." << std::endl
              << "The backup of keys is YOUR responsibility." << std::endl
              << "" << std::endl
              << BrightRedMsg("If you lose your seed, your account can not be recovered.") << std::endl
              << "" << std::endl
              << BrightYellowMsg("The Lithe Projects Team doesn't take any responsibility for lost") << std::endl
              << BrightYellowMsg("funds due to nonexistent/missing/lost private keys.") << std::endl
              << "" << std::endl;

    std::cout << "Wallet Address: " << BrightMagentaMsg(m_wallet->getAddress()) << std::endl;
    std::cout << "Private spend key: " << BrightMagentaMsg(Common::podToHex(keys.spendSecretKey)) << std::endl;
    std::cout << "Private view key: " << BrightMagentaMsg(Common::podToHex(keys.viewSecretKey)) << std::endl;
    std::cout << "Mnemonic Seed: " << BrightMagentaMsg(generate_mnemonic(keys.spendSecretKey)) << std::endl;
  } catch (const std::exception& e) {
    logger(DEBUGGING) << "Failed to generate a new wallet: " << e.what();
    std::cout << RedMsg("Failed to generate a new wallet: ") << YellowMsg(e.what()) << std::endl;
    return false;
  }

  std::cout << std::endl
            << BrightGreenMsg("Congratulations, your wallet has been created!") << std::endl
            << std::endl
            << BrightYellowMsg("You should always use \"exit\" command when closing lithe-wallet to save") << std::endl
            << BrightYellowMsg("your current session's state.") << std::endl
            << BrightYellowMsg("Otherwise, you will possibly need to re-synchronize your chain.") << std::endl
            << std::endl
            << YellowMsg("If you forget to use exit, your wallet is not at risk in anyway.") << std::endl;

  if(m_exit_after_generate) {
    m_consoleHandler.requestStop();
    std::exit(0);
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::new_wallet(Crypto::SecretKey &secret_key, Crypto::SecretKey &view_key, const std::string &wallet_file, const std::string& password) {
  m_wallet_file = wallet_file;
  
  m_wallet.reset(new WalletLegacy(m_currency, *m_node.get(), logManager));
  m_node->addObserver(static_cast<INodeObserver*>(this));
  m_wallet->addObserver(this);
  try {
    m_initResultPromise.reset(new std::promise<std::error_code>());
    std::future<std::error_code> f_initError = m_initResultPromise->get_future();
    
    AccountKeys wallet_keys;
    wallet_keys.spendSecretKey = secret_key;
    wallet_keys.viewSecretKey = view_key;
    Crypto::secret_key_to_public_key(wallet_keys.spendSecretKey, wallet_keys.address.spendPublicKey);
    Crypto::secret_key_to_public_key(wallet_keys.viewSecretKey, wallet_keys.address.viewPublicKey);

    m_wallet->initWithKeys(wallet_keys, password);
    auto initError = f_initError.get();
    m_initResultPromise.reset(nullptr);
    if (initError) {
      logger(DEBUGGING) << "Failed to generate a new wallet: " << initError.message();
      std::cout << RedMsg("Failed to generate a new wallet: ") << YellowMsg(initError.message()) << std::endl;
      return false;
    }
    
    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (std::exception& e) {
      logger(DEBUGGING) << "Failed to save a new wallet: " << e.what();
      std::cout << RedMsg("Failed to save a new wallet: ") << YellowMsg(e.what()) << std::endl;
      throw;
    }
    
    AccountKeys keys;
    m_wallet->getAccountKeys(keys);
    
    std::cout << BrightGreenMsg("Imported Wallet: ") << BrightMagentaMsg(m_wallet->getAddress()) << std::endl;
  } catch (const std::exception& e) {
    logger(DEBUGGING) << "Failed to import wallet: " << e.what();
    std::cout << RedMsg("Failed to import wallet: ") << YellowMsg(e.what()) << std::endl;
    return false;
  }
  
  std::cout << BrightGreenMsg("Your Wallet has successfully been imported.") << std::endl << std::endl
            << BrightGreenMsg("Use \"help\" command to see the list of available commands.") << std::endl << std::endl
            << BrightYellowMsg("Always use \"exit\" command when closing simplewallet to save") << std::endl
            << BrightYellowMsg("current session's state. Otherwise, you will possibly need to synchronize") << std::endl
            << BrightYellowMsg("your wallet again. Your wallet key is NOT under risk anyway.") << std::endl << std::endl;
  
  if(m_exit_after_generate) {
    m_consoleHandler.requestStop();
    std::exit(0);
  }

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::close_wallet()
{
  try {
    CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    std::cout << BrightGreenMsg("Wallet Data saved successfully.") << std::endl;
  } catch (const std::exception& e) {
      logger(DEBUGGING) << e.what();
      std::cout << RedMsg(e.what()) << std::endl;
    return false;
  }

  m_wallet->removeObserver(this);
  m_wallet->shutdown();

  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::save(const std::vector<std::string> &args)
{
  try {
    CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    std::cout << BrightGreenMsg("Wallet Data saved successfully.") << std::endl;
  } catch (const std::exception& e) {
    logger(DEBUGGING) << e.what();
    std::cout << RedMsg(e.what()) << std::endl;
  }

  return true;
}

bool simple_wallet::reset(const std::vector<std::string> &args) {
  {
    std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
    m_walletSynchronized = false;
  }

  if(0 == args.size()) {
    std::cout << GreenMsg("Resetting wallet from Block Height 0.") << std::endl;
    m_wallet->syncAll(true, 0);
    m_wallet->reset();
    std::cout << BrightGreenMsg("Reset has successfully been completed.") << std::endl;
  } else {
    uint64_t height = 0;
    bool ok = Common::fromString(args[0], height);
    if (ok && ok <= m_node->getLastLocalBlockHeight()) {
      std::cout << GreenMsg("Resetting wallet from Block Height ") << MagentaMsg(std::to_string(height)) << std::endl;
      m_wallet->syncAll(true, height);
      m_wallet->reset(height);
      std::cout << BrightGreenMsg("Reset has successfully been completed.") << std::endl;
    } else if (ok > m_node->getLastLocalBlockHeight()) {
      std::cout << BrightRedMsg("Whoops! That block hasn't been passed through the Blockchain yet.") << std::endl
                << BrightRedMsg("Please try using a lower Block Height.") << std::endl;
      return false;
    }
  }

  std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  while (!m_walletSynchronized) {
    m_walletSynchronizedCV.wait(lock);
  }

  std::cout << std::endl;

  return true;
}

bool simple_wallet::start_mining(const std::vector<std::string>& args) {
  COMMAND_RPC_START_MINING::request req;
  req.miner_address = m_wallet->getAddress();

  bool ok = true;
  uint64_t max_mining_threads_count = (std::max)(std::thread::hardware_concurrency(), static_cast<unsigned>(2));
  if (0 == args.size()) {
    req.threads_count = 1;
  } else if (1 == args.size()) {
    uint16_t num = 1;
    ok = Common::fromString(args[0], num);
    ok = ok && (1 <= num && num <= max_mining_threads_count);
    req.threads_count = num;
  } else {
    ok = false;
  }

  if (!ok) {
    logger(DEBUGGING) << "User tried to use the wrong arguments with start_mining";
    std::cout << RedMsg("Invalid arguments used.") << std::endl
              << RedMsg("Please use \"start_mining <numberOfThreads>\".") << std::endl
              << YellowMsg("<numberOfThreads> should be from 1 to ")
              << YellowMsg(std::to_string(max_mining_threads_count)) << std::endl;
    return true;
  }


  COMMAND_RPC_START_MINING::response res;

  try {
    HttpClient httpClient(m_dispatcher, m_daemon_host, m_daemon_port);
    invokeJsonCommand(httpClient, "/start_mining", req, res);

    std::string err = interpret_rpc_response(true, res.status);

    if (err.empty()) {
      std::cout << BrightGreenMsg("Mining has successfully started in the daemon.") << std::endl;
    } else {
      logger(DEBUGGING) << "Mining could not start: " << err;
      std::cout << RedMsg("Mining could not start: ") << RedMsg(err) << std::endl;
    }
  } catch (const ConnectException&) {
    printConnectionError();
  } catch (const std::exception& e) {
    logger(DEBUGGING) << "Failed to invoke RPC method: " << e.what();
    std::cout << RedMsg("Failed to invoke RPC method: ") << YellowMsg(e.what()) << std::endl;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::stop_mining(const std::vector<std::string>& args)
{
  COMMAND_RPC_STOP_MINING::request req;
  COMMAND_RPC_STOP_MINING::response res;

  try {
    HttpClient httpClient(m_dispatcher, m_daemon_host, m_daemon_port);

    invokeJsonCommand(httpClient, "/stop_mining", req, res);
    std::string err = interpret_rpc_response(true, res.status);
    if (err.empty()){
      std::cout << BrightGreenMsg("Mining has successfully been stopped.") << std::endl;
    } else {
      logger(DEBUGGING) << "Mining has not been stopped: " << err;
      std::cout << BrightGreenMsg("Mining has not been stopped: ") << RedMsg(err) << std::endl;
    }
  } catch (const ConnectException&) {
    printConnectionError();
  } catch (const std::exception& e) {
    logger(DEBUGGING) << "Failed to invoke RPC method: " << e.what();
    std::cout << RedMsg("Failed to invoke RPC method: ") << YellowMsg(e.what()) << std::endl;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::initCompleted(std::error_code result) {
  if (m_initResultPromise.get() != nullptr) {
    m_initResultPromise->set_value(result);
  }
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::connectionStatusUpdated(bool connected) {
  if (connected) {
    std::cout << BrightGreenMsg("The Wallet is now connected with the Daemon.") << std::endl;
  } else {
    printConnectionError();
  }
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::externalTransactionCreated(CryptoNote::TransactionId transactionId)  {
  WalletLegacyTransaction txInfo;
  m_wallet->getTransaction(transactionId, txInfo);

  /* this tells the wallet to show incoming+outgoing transactions live */
  if (txInfo.totalAmount >= 0) {
    std::cout << std::endl
              << BrightGreenMsg("New Transaction Found:") << std::endl
              << BrightGreenMsg("Height: ") << BrightMagentaMsg(std::to_string(txInfo.blockHeight)) << std::endl
              << BrightGreenMsg("Transaction: ") << BrightMagentaMsg(Common::podToHex(txInfo.hash)) << std::endl
              << BrightGreenMsg("Amount: ") << BrightMagentaMsg(m_currency.formatAmount(txInfo.totalAmount)) << std::endl;
  } else {
    std::cout << std::endl
              << BrightGreenMsg("Outgoing Transaction Found:") << std::endl
              << BrightGreenMsg("Height: ") << BrightMagentaMsg(std::to_string(txInfo.blockHeight)) << std::endl
              << BrightGreenMsg("Transaction: ") << BrightMagentaMsg(Common::podToHex(txInfo.hash)) << std::endl
              << BrightGreenMsg("Spent: ") << BrightMagentaMsg(m_currency.formatAmount(static_cast<uint64_t>(-txInfo.totalAmount))) << std::endl;
  }
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::synchronizationCompleted(std::error_code result) {
  std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  m_walletSynchronized = true;
  m_walletSynchronizedCV.notify_one();
}

void simple_wallet::synchronizationProgressUpdated(uint32_t current, uint32_t total) {
  std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
  if (!m_walletSynchronized) {
    m_refresh_progress_reporter.update(current, false);
  }
}

bool simple_wallet::show_balance(const std::vector<std::string>& args) {
  Table balTab;
  balTab.add_row({"Available Balance: " + m_currency.formatAmount(m_wallet->actualBalance()) + " $LXTH"});
  balTab.add_row({"Pending Balance: " + m_currency.formatAmount(m_wallet->pendingBalance()) + " $LXTH"});
  balTab.add_row({"Total Balance: " + m_currency.formatAmount(m_wallet->actualBalance() + m_wallet->pendingBalance()) + " $LXTH"}); 

  balTab.format()
    .font_align(FontAlign::center)
    .border_top("═").border_bottom("═").border_left("║").border_right("║")
    .corner_top_left("╔").corner_top_right("╗").corner_bottom_left("╚").corner_bottom_right("╝");

  balTab.row(1).format().corner_top_left("╠").corner_top_right("╣");
  balTab.row(2).format().corner_top_left("╠").corner_top_right("╣");

  balTab.row(0).format().font_align(FontAlign::center).font_color(Color::green);
  balTab.row(1).format().font_align(FontAlign::center).font_color(Color::yellow);
  balTab.row(2).format().font_align(FontAlign::center).font_color(Color::green);

  std::cout << balTab << std::endl;
  return true;
}

bool simple_wallet::sign_message(const std::vector<std::string>& args) {
  if(args.size() < 1) {
    std::cout << RedMsg("Use: \"sign_message <message>\".") << std::endl;
    return true;
  }
    
  AccountKeys keys;
  m_wallet->getAccountKeys(keys);

  Crypto::Hash message_hash;
  Crypto::Signature sig;
  Crypto::cn_fast_hash(args[0].data(), args[0].size(), message_hash);
  Crypto::generate_signature(message_hash, keys.address.spendPublicKey, keys.spendSecretKey, sig);
  
  std::cout << BrightGreenMsg("Sig ") << BrightGreenMsg(Tools::Base58::encode(std::string(reinterpret_cast<char*>(&sig)))) << std::endl;

  return true;	
}

bool simple_wallet::verify_signature(const std::vector<std::string>& args) {
  if (args.size() != 3) {
    logger(DEBUGGING) << "User used not enough arguments or too many.";
    std::cout << RedMsg("Use: \"verify_signature <message> <address> <signature>\".") << std::endl;
    return true;
  }
  
  std::string encodedSig = args[2];
  const uint64_t prefix_size = strlen("Sig");
  
  if (encodedSig.substr(0, prefix_size) != "Sig") {
    logger(DEBUGGING) << "Invalid signature prefix.";
    std::cout << RedMsg("Invalid signature prefix.") << std::endl;
    return true;
  } 
  
  Crypto::Hash message_hash;
  Crypto::cn_fast_hash(args[0].data(), args[0].size(), message_hash);
  
  std::string decodedSig;
  Crypto::Signature sig;
  Tools::Base58::decode(encodedSig.substr(prefix_size), decodedSig);
  memcpy(&sig, decodedSig.data(), sizeof(sig));
  
  uint64_t prefix;
  CryptoNote::AccountPublicAddress addr;
  CryptoNote::parseAccountAddressString(prefix, addr, args[1]);
  
  if (Crypto::check_signature(message_hash, addr.spendPublicKey, sig)) {
    std::cout << BrightGreenMsg("Valid Signature.") << std::endl;
  } else {
    logger(DEBUGGING) << "Invalid signature given.";
    std::cout << RedMsg("Invalid Signature.") << std::endl;
  }
  return true;
}

/* ------------------------------------------------------------------------------------------- */

/* CREATE INTEGRATED ADDRESS */
/* take a payment Id as an argument and generate an integrated wallet address */

bool simple_wallet::create_integrated(const std::vector<std::string>& args) {
  /* check if there is a payment id */
  if (args.empty()) {
    logger(DEBUGGING) << "User provided no Payment ID even though one is needeed.";
    std::cout << RedMsg("Please enter a Payment ID.") << std::endl;
    return true;
  }

  std::string paymentID = args[0];
  std::regex hexChars("^[0-9a-f]+$");
  if(paymentID.size() != 64 || !regex_match(paymentID, hexChars)) {
    logger(DEBUGGING) << "User provided an invalid Payment ID.";
    std::cout << RedMsg("Invalid Payment ID.") << std::endl;
    return true;
  }

  std::string address = m_wallet->getAddress();
  uint64_t prefix;
  CryptoNote::AccountPublicAddress addr;

  /* get the spend and view public keys from the address */
  if(!CryptoNote::parseAccountAddressString(prefix, addr, address)) {
    logger(DEBUGGING) << "Failed to parse account address from string.";
    std::cout << RedMsg("Failed to parse account address from string.") << std::endl;
    return true;
  }

  CryptoNote::BinaryArray ba;
  CryptoNote::toBinaryArray(addr, ba);
  std::string keys = Common::asString(ba);

  /* create the integrated address the same way you make a public address */
  std::string integratedAddress = Tools::Base58::encode_addr (CryptoNote::parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX,
                                                              paymentID + keys
  );

  std::cout << std::endl
            << BrightGreenMsg("Integrated address: ") << BrightMagentaMsg(integratedAddress)
            << std::endl << std::endl;

  return true;
}

/* ---------------------------------------------------------------------------------------- */


bool simple_wallet::export_keys(const std::vector<std::string>& args/* = std::vector<std::string>()*/) {
  AccountKeys keys;
  m_wallet->getAccountKeys(keys);

  std::string secretKeysData = std::string(reinterpret_cast<char*>(&keys.spendSecretKey), sizeof(keys.spendSecretKey)) + std::string(reinterpret_cast<char*>(&keys.viewSecretKey), sizeof(keys.viewSecretKey));
  std::string guiKeys = Tools::Base58::encode_addr(CryptoNote::parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX, secretKeysData);


  std::cout << "" << std::endl
            << BrightMagentaMsg("lithe-wallet is an open-source, client-side, free wallet which") << std::endl
            << BrightMagentaMsg("allows you to send and receive $LXTH instantly on the blockchain.") << std::endl
            << "" << std::endl
            << "You are in control of your funds & your keys." << std::endl
            << "" << std::endl
            << "When you generate a new wallet, login, send, receive or deposit $LXTH - everything happens locally." << std::endl
            << "" << std::endl
            << "Your seed is never transmitted, received or stored - anywhere." << std::endl
            << "That's why its imperative to write, print or save your seed somewhere safe." << std::endl
            << "The backup of keys is YOUR responsibility." << std::endl
            << "" << std::endl
            << BrightRedMsg("If you lose your seed, your account can not be recovered.") << std::endl
            << "" << std::endl
            << BrightYellowMsg("The Lithe Projects Team doesn't take any responsibility for lost") << std::endl
            << BrightYellowMsg("funds due to nonexistent/missing/lost private keys.") << std::endl
            << "" << std::endl;

  std::cout << "Private spend key: " << Common::podToHex(keys.spendSecretKey) << std::endl;
  std::cout << "Private view key: " <<  Common::podToHex(keys.viewSecretKey) << std::endl;

  Crypto::PublicKey unused_dummy_variable;
  Crypto::SecretKey deterministic_private_view_key;

  AccountBase::generateViewFromSpend(keys.spendSecretKey, deterministic_private_view_key, unused_dummy_variable);

  bool deterministic_private_keys = deterministic_private_view_key == keys.viewSecretKey;

/* dont show a mnemonic seed if it is an old non-deterministic wallet */
  if (deterministic_private_keys) {
    std::cout << "Mnemonic seed: " << generate_mnemonic(keys.spendSecretKey) << std::endl << std::endl;
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_incoming_transfers(const std::vector<std::string>& args) {
  bool hasTransfers = false;
  uint64_t transactionsCount = m_wallet->getTransactionCount();
  for (uint64_t trantransactionNumber = 0; trantransactionNumber < transactionsCount; ++trantransactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(trantransactionNumber, txInfo);
    if (txInfo.totalAmount < 0) continue;
    hasTransfers = true;
    std::cout << BrightMagentaMsg("        amount       \t                              tx id") << std::endl;
    std::cout << std::setw(21) << BrightGreenMsg(m_currency.formatAmount(txInfo.totalAmount)) << "\t"
              << BrightGreenMsg(Common::podToHex(txInfo.hash)) << std::endl;
  }

  if (!hasTransfers) std::cout << GreenMsg("No incoming transfers.");
  return true;
}

//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_outgoing_transfers(const std::vector<std::string>& args) {
  bool hasTransfers = false;
  uint64_t transactionsCount = m_wallet->getTransactionCount();
  for (uint64_t trantransactionNumber = 0; trantransactionNumber < transactionsCount; ++trantransactionNumber) {
    WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(trantransactionNumber, txInfo);
    if (txInfo.totalAmount > 0) continue;
    hasTransfers = true;
    std::cout << BrightMagentaMsg("        amount       \t                              tx id") << std::endl;
    std::cout << std::setw(21) << BrightYellowMsg(m_currency.formatAmount(txInfo.totalAmount)) << "\t"
              << BrightYellowMsg(Common::podToHex(txInfo.hash)) << std::endl;
  }

  if (!hasTransfers) std::cout << GreenMsg("No outgoing transfers.");
  return true;
}

bool simple_wallet::listTransfers(const std::vector<std::string>& args) {
  bool haveTransfers = false;
  bool haveBlockHeight = false;
  std::string blockHeightString = ""; 
  uint32_t blockHeight = 0;
  WalletLegacyTransaction txInfo;


  /* get block height from arguments */
  if (args.empty()) 
  {
    haveBlockHeight = false;
  } else {
    blockHeightString = args[0];
    haveBlockHeight = true;
    blockHeight = atoi(blockHeightString.c_str());
  }

  uint64_t transactionsCount = m_wallet->getTransactionCount();
  for (uint64_t trantransactionNumber = 0; trantransactionNumber < transactionsCount; ++trantransactionNumber) 
  {
    
    m_wallet->getTransaction(trantransactionNumber, txInfo);
    if (txInfo.state != WalletLegacyTransactionState::Active || txInfo.blockHeight == WALLET_LEGACY_UNCONFIRMED_TRANSACTION_HEIGHT) {
      continue;
    }

    if (!haveTransfers) {
      printListTransfersHeader(logger);
      haveTransfers = true;
    }

    if (haveBlockHeight == false) {
      printListTransfersItem(logger, txInfo, *m_wallet, m_currency);
    } else {
      if (txInfo.blockHeight >= blockHeight) {
        printListTransfersItem(logger, txInfo, *m_wallet, m_currency);
      }
    }
  }

  if (!haveTransfers) std::cout << GreenMsg("No transfers");

  return true;
}

bool simple_wallet::show_payments(const std::vector<std::string> &args) {
  if (args.empty()) {
    logger(DEBUGGING) << "Expected at least one Payment ID.";
    std::cout << RedMsg("Expected at least one Payment ID.") << std::endl;
    return true;
  }

  try {
    auto hashes = args;
    std::sort(std::begin(hashes), std::end(hashes));
    hashes.erase(std::unique(std::begin(hashes), std::end(hashes)), std::end(hashes));
    std::vector<PaymentId> paymentIds;
    paymentIds.reserve(hashes.size());
    std::transform(std::begin(hashes), std::end(hashes), std::back_inserter(paymentIds), [](const std::string& arg) {
      PaymentId paymentId;
      if (!CryptoNote::parsePaymentId(arg, paymentId)) {
        throw std::runtime_error("payment ID has invalid format: \"" + arg + "\", expected 64-character string");
      }

      return paymentId;
    });

    //logger(DEBUGGING) << "                            payment                             \t" <<
    //  "                          transaction                           \t" <<
    //  "  height\t       amount        ";
    /* next 3 lines havent been tested for layout, just going from above 3 lines */
    std::cout << GreenMsg("                            payment                             \t") <<
      GreenMsg("                          transaction                           \t") <<
      GreenMsg("  height\t       amount        ") << std::endl;

    auto payments = m_wallet->getTransactionsByPaymentIds(paymentIds);

    for (auto& payment : payments) {
      for (auto& transaction : payment.transactions) {
        std::cout << BrightGreenMsg(Common::podToHex(payment.paymentId)) << '\t'
                  << BrightGreenMsg(Common::podToHex(transaction.hash)) << '\t'
                  << std::setw(8) << BrightGreenMsg(std::to_string(transaction.blockHeight)) << '\t'
                  << std::setw(21) << BrightGreenMsg(m_currency.formatAmount(transaction.totalAmount));
      }

      if (payment.transactions.empty()) {
        std::cout << YellowMsg("No payments with ID: ") << BrightYellowMsg(Common::podToHex(payment.paymentId)) << std::endl;
      }
    }
  } catch (std::exception& e) {
    logger(DEBUGGING) << "show_payments exception: " << e.what();
    std::cout << RedMsg("show_payments exception: ") << RedMsg(e.what()) << std::endl;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_wallet_info(const std::vector<std::string>& args) {
  try {
    uint64_t wal_height = m_node->getLastLocalBlockHeight();

    std::cout << "Wallet Height: " << wal_height << std::endl
              << "Wallet Type: " << (m_currency.isTestnet() ? "Testnet" : "Mainnet") << std::endl;
    //@TODO work in bc_height and make sync bool
  } catch (std::exception &e) {
    logger(DEBUGGING) << "Failed to get Wallet Information: " << e.what();
    std::cout << RedMsg("Failed to get Wallet Information: ") << RedMsg(e.what());
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::show_num_unlocked_outputs(const std::vector<std::string>& args) {
  try {
    std::vector<TransactionOutputInformation> unlocked_outputs = m_wallet->getUnspentOutputs();
    std::cout << BrightGreenMsg("Count: ") << BrightMagentaMsg(std::to_string(unlocked_outputs.size())) << std::endl;
    for (const auto& out : unlocked_outputs) {
      std::cout << BrightGreenMsg("Key: ") << out.transactionPublicKey << BrightMagentaMsg(m_currency.formatAmount(out.amount)) << std::endl;
    }
  } catch (std::exception &e) {
    logger(DEBUGGING) << "Failed to get Outputs: " << e.what();
    std::cout << RedMsg("Failed to get Outputs: ") << RedMsg(e.what()) << std::endl;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::optimize_outputs(const std::vector<std::string>& args) {
  try {
    CryptoNote::WalletHelper::SendCompleteResultObserver sent;
    WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);

    std::vector<CryptoNote::WalletLegacyTransfer> transfers;
    std::vector<CryptoNote::TransactionMessage> messages;
    std::string extraString;
    uint64_t fee = CryptoNote::parameters::MINIMUM_FEE;
    uint64_t mixIn = 0;
    uint64_t unlockTimestamp = 0;
    uint64_t ttl = 0;
    Crypto::SecretKey transactionSK;
    CryptoNote::TransactionId tx = m_wallet->sendTransaction(transactionSK, transfers, fee, extraString, mixIn, unlockTimestamp, messages, ttl);
    if (tx == WALLET_LEGACY_INVALID_TRANSACTION_ID) {
      logger(DEBUGGING) << "User tried to send money uses legacy invalid tx id.";
      std::cout << RedMsg("Can't send money") << std::endl;
      return true;
    }

    std::error_code sendError = sent.wait(tx);
    removeGuard.removeObserver();

    if (sendError) {
      logger(DEBUGGING) << sendError.message();
      std::cout << RedMsg(sendError.message()) << std::endl;
      return true;
    }

    CryptoNote::WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(tx, txInfo);

    std::cout << BrightGreenMsg("Money has been successfully sent.") << std::endl
              << BrightGreenMsg("Transaction: ") << BrightMagentaMsg(Common::podToHex(txInfo.hash)) << std::endl
              << BrightGreenMsg("Transaction Secret Key: ") << BrightMagentaMsg(Common::podToHex(transactionSK)) << std::endl;

    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (const std::exception& e) {
      logger(DEBUGGING) << e.what();
      std::cout << RedMsg(e.what()) << std::endl;
      return true;
    }
  } catch (const std::system_error& e) {
      logger(DEBUGGING) << e.what();
    std::cout << RedMsg(e.what()) << std::endl;
  } catch (const std::exception& e) {
      logger(DEBUGGING) << e.what();
    std::cout << RedMsg(e.what()) << std::endl;
  } catch (...) {
      logger(DEBUGGING) << "Unknown error!";
    std::cout << RedMsg("Unknown error!") << std::endl;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::optimize_all_outputs(const std::vector<std::string>& args) {

  uint64_t num_unlocked_outputs = 0;

  try {
    num_unlocked_outputs = m_wallet->getNumUnlockedOutputs();
    std::cout << GreenMsg("Total Outputs: ") << MagentaMsg(std::to_string(num_unlocked_outputs)) << std::endl;
  } catch (std::exception &e) {
    logger(DEBUGGING) << "Failed to get Outputs: " << e.what();
    std::cout << RedMsg("Failed to get Uutputs: ") << RedMsg(e.what()) << std::endl;
  }

  uint64_t remainder = num_unlocked_outputs % 100;
  uint64_t rounds = (num_unlocked_outputs - remainder) / 100;
  std::cout << GreenMsg("Total Optimization Rounds: ") << MagentaMsg(std::to_string(rounds)) << std::endl;
  for(uint64_t a = 1; a < rounds; a = a + 1 ) {
    try {
      CryptoNote::WalletHelper::SendCompleteResultObserver sent;
      WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);

      std::vector<CryptoNote::WalletLegacyTransfer> transfers;
      std::vector<CryptoNote::TransactionMessage> messages;
      std::string extraString;
      uint64_t fee = CryptoNote::parameters::MINIMUM_FEE;
      uint64_t mixIn = 0;
      uint64_t unlockTimestamp = 0;
      uint64_t ttl = 0;
      Crypto::SecretKey transactionSK;
      CryptoNote::TransactionId tx = m_wallet->sendTransaction(transactionSK, transfers, fee, extraString, mixIn, unlockTimestamp, messages, ttl);
      if (tx == WALLET_LEGACY_INVALID_TRANSACTION_ID) {
        logger(DEBUGGING) << "Legacy invalid tx id used.";
        std::cout << RedMsg("Can't send money.") << std::endl;
        return true;
      }

      std::error_code sendError = sent.wait(tx);
      removeGuard.removeObserver();

      if (sendError) {
        logger(DEBUGGING) << sendError.message();
        std::cout << RedMsg(sendError.message()) << std::endl;
        return true;
      }

      CryptoNote::WalletLegacyTransaction txInfo;
      m_wallet->getTransaction(tx, txInfo);
      std::cout << BrightMagentaMsg(std::to_string(a)) << BrightGreenMsg(". Optimization Transaction has successfully sent.") << std::endl
                << BrightGreenMsg("Transaction: ") << BrightMagentaMsg(Common::podToHex(txInfo.hash)) << std::endl;
      try {
        CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
      } catch (const std::exception& e) {
        logger(DEBUGGING) << e.what();
        std::cout << RedMsg(e.what()) << std::endl;
        return true;
      }
    } catch (const std::system_error& e) {
        logger(DEBUGGING) << e.what();
      std::cout << RedMsg(e.what()) << std::endl;
    } catch (const std::exception& e) {
        logger(DEBUGGING) << e.what();
      std::cout << RedMsg(e.what()) << std::endl;
    } catch (...) {
        logger(DEBUGGING) << "Unknown error!";
      std::cout << RedMsg("Unknown error!") << std::endl;
    }
  }
  return true;
}
//----------------------------------------------------------------------------------------------------
std::string simple_wallet::resolveAlias(const std::string& aliasUrl) {
  std::string host;
	std::string uri;
	std::vector<std::string>records;
	std::string address;

	if (!Common::fetch_dns_txt(aliasUrl, records)) {
		throw std::runtime_error("Failed to lookup DNS record");
	}

	for (const auto& record : records) {
		if (processServerAliasResponse(record, address)) {
			return address;
		}
	}
	throw std::runtime_error("Failed to parse server response");
}
//----------------------------------------------------------------------------------------------------

/* This extracts the fee address from the remote node */
std::string simple_wallet::getFeeAddress() {
  
  HttpClient httpClient(m_dispatcher, m_daemon_host, m_daemon_port);

  HttpRequest req;
  HttpResponse res;

  req.setUrl("/feeaddress");
  try {
	  httpClient.request(req, res);
  }
  catch (const std::exception& e) {
    logger(DEBUGGING) << "Error connecting to the Remote Node: " << e.what();
    std::cout << RedMsg("Error connecting to the Remote Node: ") << YellowMsg(e.what()) << std::endl;
  }

  if (res.getStatus() != HttpResponse::STATUS_200) {
    logger(DEBUGGING) << "Remote Node returned code: " << std::to_string(res.getStatus());
    std::cout << RedMsg("Remote Node returned code: ") << YellowMsg(std::to_string(res.getStatus())) << std::endl;
  }

  std::string address;
  if (!processServerFeeAddressResponse(res.getBody(), address)) {
    logger(DEBUGGING) << "Failed to parse Remote Node response.";
    std::cout << RedMsg("Failed to parse Remote Node response.") << std::endl;
  }

  return address;
}

bool simple_wallet::confirmTransaction(TransferCommand cmd, bool multiAddress) {
  std::string feeString;

  if (cmd.fee == 100) {
    feeString = "0.001 $LXTH";
  } else {
    feeString = m_currency.formatAmount(cmd.fee) + " $LXTH";
  }

  std::string walletName = boost::filesystem::change_extension(m_wallet_file, "").string();

  std::cout << std::endl << "Confirm Transaction?" << std::endl;

  if (!multiAddress) {
    std::cout << "You are sending " << m_currency.formatAmount(cmd.dsts[0].amount)
              << " $LXTH, with a fee of " << feeString << std::endl
              << "FROM: " << walletName << std::endl
              << "TO: " << std::endl << cmd.dsts[0].address << std::endl
              << std::endl;
  } else {
    std::cout << "You are sending a transaction to "
              << std::to_string(cmd.dsts.size())
              << " addresses, with a combined fee of " << feeString
              << " $LXTH" << std::endl << std::endl;

    for (auto destination : cmd.dsts) {
      std::cout << "You are sending " << m_currency.formatAmount(destination.amount)
                << " $LXTH" << std::endl << "FROM: " << walletName << std::endl
                << "TO: " << std::endl << destination.address << std::endl
                << std::endl;
    }
  }

  while (true) {
    std::cout << "Is this correct? (Y/N): ";

    char c;

    std::cin >> c;

    c = std::tolower(c);

    if (c == 'y') {
      if (!m_pwd_container.read_and_validate()) {
        std::cout << "Incorrect password!" << std::endl;
        continue;
      }

      return true;

    } else if (c == 'n') {
      return false;
    } else {
      std::cout << "Bad input, please enter either Y or N." << std::endl;
    }
  }

  /* Because the compiler is dumb */
  return false;
}

bool simple_wallet::transfer(const std::vector<std::string> &args) {
  try {
    TransferCommand cmd(m_currency);

    if (!cmd.parseArguments(logger, args))
      return true;

    for (auto& kv: cmd.aliases) {
      std::string address;

      try {
        address = resolveAlias(kv.first);

        AccountPublicAddress ignore;
        if (!m_currency.parseAccountAddressString(address, ignore)) {
          throw std::runtime_error("Address \"" + address + "\" is invalid");
        }
      } catch (std::exception& e) {
        logger(DEBUGGING) << "Couldn't resolve alias: " << e.what() << " Alias: " << kv.first;
        std::cout << RedMsg("Couldn't resolve alias: ") << YellowMsg(e.what()) << std::endl
                  << RedMsg("Alias: ") << YellowMsg(kv.first) << std::endl;
        return true;
      }

      for (auto& transfer: kv.second) {
        transfer.address = address;
      }
    }

    if (!cmd.aliases.empty()) {
      if (!askAliasesTransfersConfirmation(cmd.aliases, m_currency)) {
        return true;
      }

      for (auto& kv: cmd.aliases) {
        std::copy(std::move_iterator<std::vector<WalletLegacyTransfer>::iterator>(kv.second.begin()),
                  std::move_iterator<std::vector<WalletLegacyTransfer>::iterator>(kv.second.end()),
                  std::back_inserter(cmd.dsts));
      }
    }

    std::vector<TransactionMessage> messages;
    for (auto dst : cmd.dsts) {
      for (auto msg : cmd.messages) {
        messages.push_back(TransactionMessage{ msg, dst.address });
      }
    }

    uint64_t ttl = 0;
    if (cmd.ttl != 0) {
      ttl = static_cast<uint64_t>(time(nullptr)) + cmd.ttl;
    }

    CryptoNote::WalletHelper::SendCompleteResultObserver sent;

    std::string extraString;
    std::copy(cmd.extra.begin(), cmd.extra.end(), std::back_inserter(extraString));

    WalletHelper::IWalletRemoveObserverGuard removeGuard(*m_wallet, sent);

    bool proceed = confirmTransaction(cmd, cmd.dsts.size() > 1);

    if (!proceed) {
      std::cout << "Cancelling transaction." << std::endl;
      return true;
    }

    /* set static mixin of 4*/
    cmd.fake_outs_count = CryptoNote::parameters::MINIMUM_MIXIN;

    /* force minimum fee */
    if (cmd.fee < CryptoNote::parameters::MINIMUM_FEE) {
      cmd.fee = CryptoNote::parameters::MINIMUM_FEE;
    }

    Crypto::SecretKey transactionSK;
    CryptoNote::TransactionId tx = m_wallet->sendTransaction(transactionSK, cmd.dsts, cmd.fee, extraString, cmd.fake_outs_count, 0, messages, ttl);
    if (tx == WALLET_LEGACY_INVALID_TRANSACTION_ID) {
      logger(DEBUGGING) << "Legacy invalid tx id used.";
      std::cout << RedMsg("Can't send money.") << std::endl;
      return true;
    }

    std::error_code sendError = sent.wait(tx);
    removeGuard.removeObserver();

    if (sendError) {
      logger(DEBUGGING) << sendError.message();
      std::cout << RedMsg(sendError.message()) << std::endl;
      return true;
    }

    CryptoNote::WalletLegacyTransaction txInfo;
    m_wallet->getTransaction(tx, txInfo);
    std::cout << "Transaction has been sent! ID:" << std::endl
              << Common::podToHex(txInfo.hash) << std::endl;

    try {
      CryptoNote::WalletHelper::storeWallet(*m_wallet, m_wallet_file);
    } catch (const std::exception& e) {
      logger(DEBUGGING) << e.what();
      std::cout << RedMsg(e.what()) << std::endl;
      return true;
    }
  } catch (const std::system_error& e) {
    logger(DEBUGGING) << e.what();
    std::cout << RedMsg(e.what()) << std::endl;
  } catch (const std::exception& e) {
    logger(DEBUGGING) << e.what();
    std::cout << RedMsg(e.what()) << std::endl;
  } catch (...) {
    logger(DEBUGGING) << "Unknown error!";
    std::cout << RedMsg("Unknown error!") << std::endl;
  }

  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::run() {
  {
    std::unique_lock<std::mutex> lock(m_walletSynchronizedMutex);
    while (!m_walletSynchronized) {
      m_walletSynchronizedCV.wait(lock);
    }
  }

  std::cout << std::endl;

  std::string addr_start = m_wallet->getAddress().substr(0, 6);
  m_consoleHandler.start(false, "[wallet " + addr_start + "]: ", Common::Console::Color::BrightYellow);
  return true;
}
//----------------------------------------------------------------------------------------------------
void simple_wallet::stop() {
  m_consoleHandler.requestStop();
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::print_address(const std::vector<std::string> &args) {
  std::cout << BrightMagentaMsg(m_wallet->getAddress()) << std::endl;
  return true;
}
//----------------------------------------------------------------------------------------------------
bool simple_wallet::process_command(const std::vector<std::string> &args) {
  return m_consoleHandler.runCommand(args);
}

void simple_wallet::printConnectionError() const {
  logger(DEBUGGING) << "Wallet failed to connect to daemon = " << m_daemon_address;
  std::cout << RedMsg("Wallet failed to connect to Daemon. ")
            << YellowMsg("(") << YellowMsg(m_daemon_address) << YellowMsg(")") << std::endl;
}


int main(int argc, char* argv[]) {
#ifdef _WIN32
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  po::options_description desc_general("General options");
  command_line::add_arg(desc_general, command_line::arg_help);
  command_line::add_arg(desc_general, command_line::arg_version);

  po::options_description desc_params("Wallet options");
  command_line::add_arg(desc_params, arg_wallet_file);
  command_line::add_arg(desc_params, arg_generate_new_wallet);
  command_line::add_arg(desc_params, arg_password);
  command_line::add_arg(desc_params, arg_daemon_address);
  command_line::add_arg(desc_params, arg_daemon_host);
  command_line::add_arg(desc_params, arg_daemon_port);
  command_line::add_arg(desc_params, arg_command);
  command_line::add_arg(desc_params, arg_log_level);
  command_line::add_arg(desc_params, arg_testnet);
  Tools::wallet_rpc_server::init_options(desc_params);
  command_line::add_arg(desc_params, arg_sync_from_zero);
  command_line::add_arg(desc_params, arg_exit_after_generate);

  po::positional_options_description positional_options;
  positional_options.add(arg_command.name, -1);

  po::options_description desc_all;
  desc_all.add(desc_general).add(desc_params);

  Logging::LoggerManager logManager;
  Logging::LoggerRef logger(logManager, "simplewallet");
  System::Dispatcher dispatcher;

  po::variables_map vm;

  bool r = command_line::handle_error_helper(desc_all, [&]() {
    po::store(command_line::parse_command_line(argc, argv, desc_general, true), vm);

    if (command_line::get_arg(vm, command_line::arg_help)) {
      CryptoNote::Currency tmp_currency = CryptoNote::CurrencyBuilder(logManager).currency();
      CryptoNote::simple_wallet tmp_wallet(dispatcher, tmp_currency, logManager);

      std::cout << "Lithe Wallet v" << PROJECT_VERSION_LONG << std::endl;
      std::cout << "Usage: lithe-wallet [--wallet-file=<file>|--generate-new-wallet=<file>] [--daemon-address=<host>:<port>] [<COMMAND>]";
      std::cout << desc_all << '\n' << tmp_wallet.get_commands_str();
      return false;
    } else if (command_line::get_arg(vm, command_line::arg_version))  {
      std::cout << "Lithe Wallet v" << PROJECT_VERSION_LONG << std::endl;
      return false;
    }

    auto parser = po::command_line_parser(argc, argv).options(desc_params).positional(positional_options);
    po::store(parser.run(), vm);
    po::notify(vm);
    return true;
  });

  if (!r)
    return 1;

  //set up logging options
  Level logLevel = static_cast<Level>(static_cast<int>(Logging::ERROR));

  if (command_line::has_arg(vm, arg_log_level)) {
    logLevel = static_cast<Level>(command_line::get_arg(vm, arg_log_level));
  }

  logManager.configure(buildLoggerConfiguration(logLevel, Common::ReplaceExtenstion(argv[0], ".log")));

  std::cout << MagentaMsg("Lithe Wallet v") << BrightMagentaMsg(PROJECT_VERSION_LONG) << std::endl;

  CryptoNote::Currency currency = CryptoNote::CurrencyBuilder(logManager).
    testnet(command_line::get_arg(vm, arg_testnet)).currency();

  if (command_line::has_arg(vm, Tools::wallet_rpc_server::arg_rpc_bind_port)) {
	  /* If the rpc interface is run, ensure that either legacy mode or an RPC
	    password is set. */
	  if (!command_line::has_arg(vm, Tools::wallet_rpc_server::arg_rpc_password) &&
		  !command_line::has_arg(vm, Tools::wallet_rpc_server::arg_rpc_legacy_security)) {
	    logger(ERROR, BRIGHT_RED) << "Required RPC password is not set.";
	    return 1;
	  }

    //runs wallet with rpc interface
    if (!command_line::has_arg(vm, arg_wallet_file)) {
      logger(ERROR, BRIGHT_RED) << "Wallet file not set.";
      return 1;
    }

    if (!command_line::has_arg(vm, arg_daemon_address)) {
      logger(ERROR, BRIGHT_RED) << "Daemon address not set.";
      return 1;
    }

    if (!command_line::has_arg(vm, arg_password)) {
      logger(ERROR, BRIGHT_RED) << "Wallet password not set.";
      return 1;
    }

    std::string wallet_file = command_line::get_arg(vm, arg_wallet_file);
    std::string wallet_password = command_line::get_arg(vm, arg_password);
    std::string daemon_address = command_line::get_arg(vm, arg_daemon_address);
    std::string daemon_host = command_line::get_arg(vm, arg_daemon_host);
    uint16_t daemon_port = command_line::get_arg(vm, arg_daemon_port);
    if (daemon_host.empty())
      daemon_host = "localhost";
    if (!daemon_port)
      daemon_port = RPC_DEFAULT_PORT;

    if (!daemon_address.empty()) {
      if (!parseUrlAddress(daemon_address, daemon_host, daemon_port)) {
        logger(ERROR, BRIGHT_RED) << "failed to parse daemon address: " << daemon_address;
        return 1;
      }
    }

    std::unique_ptr<INode> node(new NodeRpcProxy(daemon_host, daemon_port));

    std::promise<std::error_code> errorPromise;
    std::future<std::error_code> error = errorPromise.get_future();
    auto callback = [&errorPromise](std::error_code e) {errorPromise.set_value(e); };
    node->init(callback);
    if (error.get()) {
      logger(ERROR, BRIGHT_RED) << ("failed to init NodeRPCProxy");
      return 1;
    }

    std::unique_ptr<IWalletLegacy> wallet(new WalletLegacy(currency, *node.get(), logManager));

    std::string walletFileName;
    try  {
      walletFileName = ::tryToOpenWalletOrLoadKeysOrThrow(logger, wallet, wallet_file, wallet_password);

      std::cout << BrightGreenMsg("Successfully loaded wallet. Here are your balances:") << std::endl;
      std::cout << GreenMsg("Avaliable Balance: ") << MagentaMsg(currency.formatAmount(wallet->actualBalance())) << std::endl
                << YellowMsg("Locked Balance: ") << MagentaMsg(currency.formatAmount(wallet->pendingBalance())) << std::endl
                << BrightGreenMsg("Total Balance: ") << BrightMagentaMsg(currency.formatAmount(wallet->actualBalance() + wallet->pendingBalance())) << std::endl;
    } catch (const std::exception& e)  {
      logger(ERROR, BRIGHT_RED) << "Wallet initialize failed: " << e.what();
      return 1;
    }

    Tools::wallet_rpc_server wrpc(dispatcher, logManager, *wallet, *node, currency, walletFileName);

    if (!wrpc.init(vm)) {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize wallet rpc server";
      return 1;
    }

    Tools::SignalHandler::install([&wrpc, &wallet] {
      wrpc.send_stop_signal();
    });

    std::cout << GreenMsg("Starting Wallet RPC Server.") << std::endl;

    wrpc.run();

    std::cout << GreenMsg("Wallet RPC Server has stopped.") << std::endl;

    try {
      std::cout << GreenMsg("Storing Wallet...") << std::endl;

      CryptoNote::WalletHelper::storeWallet(*wallet, walletFileName);

      std::cout << BrightGreenMsg("Successfully stored the Wallet.") << std::endl;
    } catch (const std::exception& e) {
      logger(ERROR, BRIGHT_RED) << "Failed to store wallet: " << e.what();
      return 1;
    }
  } else {
    //runs wallet with console interface
    CryptoNote::simple_wallet wal(dispatcher, currency, logManager);

    if (!wal.init(vm)) {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize wallet";
      return 1;
    }

    std::vector<std::string> command = command_line::get_arg(vm, arg_command);
    if (!command.empty())
      wal.process_command(command);

    Tools::SignalHandler::install([&wal] {
      wal.stop();
    });

    wal.run();

    if (!wal.deinit()) {
      logger(ERROR, BRIGHT_RED) << "Failed to close wallet";
    } else {
      logger(TRACE) << "Wallet closed";
      std::cout << GreenMsg("The Wallet has been closed.") << std::endl;
    }
  }
  return 1;
  //CATCH_ENTRY_L0("main", 1);
}