// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 Conceal Network & Conceal Devs
// Copyright (c) 2019-2020 The Lithe Project Development Team

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ctime>

#include "DaemonCommandsHandler.h"

#include "P2p/NetNode.h"
#include "CryptoNoteCore/Miner.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "Serialization/SerializationTools.h"
#include "version.h"

#include "Common/ColouredMsg.h"
#include <boost/format.hpp>
#include "Rpc/RpcServer.h"

#include <tabulate/table.hpp>
using namespace tabulate;

namespace {
  template <typename T>
  static bool print_as_json(const T& obj) {
    std::cout << CryptoNote::storeToJson(obj) << ENDL;
    return true;
  }
}

DaemonCommandsHandler::DaemonCommandsHandler(CryptoNote::core& core, CryptoNote::NodeServer& srv, Logging::LoggerManager& log, CryptoNote::RpcServer* prpc_server) :
  m_core(core), m_srv(srv), logger(log, "daemon"), m_logManager(log), m_prpc_server(prpc_server) {
  m_consoleHandler.setHandler("help", boost::bind(&DaemonCommandsHandler::help, this, _1), "Show Basic Commands");
  m_consoleHandler.setHandler("advanced", boost::bind(&DaemonCommandsHandler::advanced, this, _1), "Show Advanced Commands");
  m_consoleHandler.setHandler("help-usage", boost::bind(&DaemonCommandsHandler::helpUsage, this, _1), "Show Basic Commands");
  m_consoleHandler.setHandler("advanced-usage", boost::bind(&DaemonCommandsHandler::advancedUsage, this, _1), "Show Advanced Commands");
  
  m_consoleHandler.setHandler("exit", boost::bind(&DaemonCommandsHandler::exit, this, _1), "Shutdown the daemon");
  m_consoleHandler.setHandler("start_mining", boost::bind(&DaemonCommandsHandler::start_mining, this, _1), "Start mining for specified address, start_mining <addr> [threads=1]");
  m_consoleHandler.setHandler("stop_mining", boost::bind(&DaemonCommandsHandler::stop_mining, this, _1), "Stop mining");
  m_consoleHandler.setHandler("show_hr", boost::bind(&DaemonCommandsHandler::show_hr, this, _1), "Start showing hash rate");
  m_consoleHandler.setHandler("hide_hr", boost::bind(&DaemonCommandsHandler::hide_hr, this, _1), "Stop showing hash rate");
  m_consoleHandler.setHandler("set_log", boost::bind(&DaemonCommandsHandler::set_log, this, _1), "set_log <level> - Change current log level, <level> is a number 0-4");
  m_consoleHandler.setHandler("status", boost::bind(&DaemonCommandsHandler::status, this, _1), "Show daemon status");

  m_consoleHandler.setHandlerAdv("print_pl", boost::bind(&DaemonCommandsHandler::print_pl, this, _1), "Print peer list");
  m_consoleHandler.setHandlerAdv("rollback_chain", boost::bind(&DaemonCommandsHandler::rollback_chain, this, _1), "Rollback chain to specific height, rollback_chain <height>");
  m_consoleHandler.setHandlerAdv("print_cn", boost::bind(&DaemonCommandsHandler::print_cn, this, _1), "Print connections");
  m_consoleHandler.setHandlerAdv("print_bc", boost::bind(&DaemonCommandsHandler::print_bc, this, _1), "Print blockchain info in a given blocks range, print_bc <begin_height> [<end_height>]");
  m_consoleHandler.setHandlerAdv("print_block", boost::bind(&DaemonCommandsHandler::print_block, this, _1), "Print block, print_block <block_hash> | <block_height>");
  m_consoleHandler.setHandlerAdv("print_stat", boost::bind(&DaemonCommandsHandler::print_stat, this, _1), "Print statistics, print_stat <nothing=last> | <block_hash> | <block_height>");
  m_consoleHandler.setHandlerAdv("print_tx", boost::bind(&DaemonCommandsHandler::print_tx, this, _1), "Print transaction, print_tx <transaction_hash>");
  m_consoleHandler.setHandlerAdv("print_pool", boost::bind(&DaemonCommandsHandler::print_pool, this, _1), "Print transaction pool (long format)");
  m_consoleHandler.setHandlerAdv("print_pool_sh", boost::bind(&DaemonCommandsHandler::print_pool_sh, this, _1), "Print transaction pool (short format)");
}

//--------------------------------------------------------------------------------
std::string DaemonCommandsHandler::get_commands_str() {
  std::stringstream ss;
  ss << CryptoNote::CRYPTONOTE_NAME << " v" << PROJECT_VERSION_LONG << ENDL;
  ss << "Basic Commands: " << ENDL;
  std::string usage = m_consoleHandler.getUsage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}
//--------------------------------------------------------------------------------
std::string DaemonCommandsHandler::get_adv_commands_str() {
  std::stringstream ss;
  ss << "Advanced Commands: " << ENDL;
  std::string usage = m_consoleHandler.getUsageAdv();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << ENDL;
  return ss.str();
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::status(const std::vector<std::string>& args) {
  CryptoNote::COMMAND_RPC_GET_INFO::request req;
  CryptoNote::COMMAND_RPC_GET_INFO::response resp;
  Table statusTable;

  /* dont show us status if status cant be done */
  if (!m_prpc_server->on_get_info(req, resp) || resp.status != CORE_RPC_STATUS_OK) {
    std::cout << "Problem retreiving information from RPC server." << std::endl;
  }

  /* uptime to string */
  std::time_t uptime = std::time(nullptr) - resp.start_time;
  std::string uptimeDay = std::to_string((unsigned int)floor(uptime / 60.0 / 60.0 / 24.0));
  std::string uptimeHrs = std::to_string((unsigned int)floor(fmod((uptime / 60.0 / 60.0), 24.0)));
  std::string uptimeMin = std::to_string((unsigned int)floor(fmod((uptime / 60.0), 60.0)));
  std::string uptimeSec = std::to_string((unsigned int)fmod(uptime, 60.0));

  /* statusTable items */
  statusTable.add_row({"Height", std::to_string(resp.height)});
  statusTable.add_row({"BC Height", std::to_string(resp.last_known_block_index)});
  statusTable.add_row({"Syned", get_sync_percentage(resp.height, resp.last_known_block_index) + "%"});
  statusTable.add_row({"Net Type", (m_core.currency().isTestnet() ? "Testnet" : "Mainnet")});
  statusTable.add_row({"Incoming", std::to_string(resp.incoming_connections_count) + " connections"});
  statusTable.add_row({"Outgoing", std::to_string(resp.outgoing_connections_count) + " connections"});
  statusTable.add_row({"Uptime", uptimeDay + "d " + uptimeHrs + "h " + uptimeMin + "m " + uptimeSec + "s"});

  /* format statusTable */
  statusTable.column(0).format().font_align(FontAlign::center).font_color(Color::green);
  statusTable.column(1).format().font_align(FontAlign::center).font_color(Color::magenta);
  statusTable.format().corner_color(Color::grey).border_color(Color::grey);

  /* print statusTable */
  std::cout << statusTable << std::endl;

  return true;
}
//--------------------------------------------------------------------------------
std::string DaemonCommandsHandler::get_mining_speed(uint32_t hr) {
  if (hr>1e9) return (boost::format("%.2f GH/s") % (hr/1e9)).str();
  if (hr>1e6) return (boost::format("%.2f MH/s") % (hr/1e6)).str();
  if (hr>1e3) return (boost::format("%.2f KH/s") % (hr/1e3)).str();
  return (boost::format("%.0f H/s") % hr).str();
}
//--------------------------------------------------------------------------------
std::string DaemonCommandsHandler::get_sync_percentage(uint64_t height, uint64_t target_height) {
  /* Don't divide by zero */
  if (height == 0 || target_height == 0) {
    return "0.00";
  }

  /* So we don't have > 100% */
  if (height > target_height) {
    height = target_height;
  }

  float percent = 100.0f * height / target_height;

  if (height < target_height && percent > 99.99f) {
    percent = 99.99f; // to avoid 100% when not fully synced
  }

  std::stringstream stream;

  stream << std::setprecision(2) << std::fixed << percent;

  return stream.str();
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::exit(const std::vector<std::string>& args) {
  m_consoleHandler.requestStop();
  m_srv.sendStopSignal();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::help(const std::vector<std::string>& args) {
  std::cout << BrightGreenMsg("Basic Commands Descriptions") << std::endl;
  showHelpTable();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::advanced(const std::vector<std::string>& args) {
  std::cout << BrightGreenMsg("Advanced Commands Descriptions") << std::endl;
  showAdvancedTable();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::helpUsage(const std::vector<std::string>& args) {
  std::cout << BrightGreenMsg("Basic Commands Usage") << std::endl;
  showHelpUsageTable();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::advancedUsage(const std::vector<std::string>& args) {
  std::cout << BrightGreenMsg("Advanced Commands Usage") << std::endl;
  showAdvancedUsageTable();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_pl(const std::vector<std::string>& args) {
  m_srv.log_peerlist();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::show_hr(const std::vector<std::string>& args) {
  if (!m_core.get_miner().is_mining()) {
    std::cout << "Mining is not started. You need to start mining before you can see hash rate." << ENDL;
  } else {
    m_core.get_miner().do_print_hashrate(true);
  }
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::hide_hr(const std::vector<std::string>& args) {
  m_core.get_miner().do_print_hashrate(false);
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_bc_outs(const std::vector<std::string>& args) {
  if (args.size() != 1) {
    std::cout << "need file path as parameter" << ENDL;
    return true;
  }
  m_core.print_blockchain_outs(args[0]);
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_cn(const std::vector<std::string>& args) {
  m_srv.get_payload_object().log_connections();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_bc(const std::vector<std::string> &args) {
  if (!args.size()) {
    std::cout << "need block index parameter" << ENDL;
    return false;
  }

  uint32_t start_index = 0;
  uint32_t end_index = 0;
  uint32_t end_block_parametr = m_core.getDaemonHeight();
  if (!Common::fromString(args[0], start_index)) {
    std::cout << "wrong starter block index parameter" << ENDL;
    return false;
  }

  if (args.size() > 1 && !Common::fromString(args[1], end_index)) {
    std::cout << "wrong end block index parameter" << ENDL;
    return false;
  }

  if (end_index == 0) {
    end_index = end_block_parametr;
  }

  if (end_index > end_block_parametr) {
    std::cout << "end block index parameter shouldn't be greater than " << end_block_parametr << ENDL;
    return false;
  }

  if (end_index <= start_index) {
    std::cout << "end block index should be greater than starter block index" << ENDL;
    return false;
  }

  m_core.print_blockchain(start_index, end_index);
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_bci(const std::vector<std::string>& args)
{
  m_core.print_blockchain_index();
  return true;
}

bool DaemonCommandsHandler::set_log(const std::vector<std::string>& args)
{
  if (args.size() != 1) {
    std::cout << "use: set_log <log_level_number_0-5>" << ENDL;
    return true;
  }

  uint16_t l = 0;
  if (!Common::fromString(args[0], l)) {
    std::cout << "wrong number format, use: set_log <log_level_number_0-4>" << ENDL;
    return true;
  }

  ++l;

  if (l > Logging::TRACE) {
    std::cout << "wrong number range, use: set_log <log_level_number_0-4>" << ENDL;
    return true;
  }

  m_logManager.setMaxLevel(static_cast<Logging::Level>(l));
  return true;
}

//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_block_by_height(uint32_t height)
{
  std::list<CryptoNote::Block> blocks;
  m_core.get_blocks(height, 1, blocks);

  if (1 == blocks.size()) {
    std::cout << "block_id: " << get_block_hash(blocks.front()) << ENDL;
    print_as_json(blocks.front());
  } else {
    uint32_t current_height;
    Crypto::Hash top_id;
    m_core.get_blockchain_top(current_height, top_id);
    std::cout << "block wasn't found. Current block chain height: " << current_height << ", requested: " << height << std::endl;
    return false;
  }

  return true;
}

//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::rollback_chain(const std::vector<std::string> &args) {
  if (args.empty()) {
    std::cout << "expected: rollback_chain <block_height>" << std::endl;
    return true;
  }

  const std::string &arg = args.front();
  uint32_t height = boost::lexical_cast<uint32_t>(arg);
  rollbackchainto(height);
  return true;
}

//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::rollbackchainto(uint32_t height)
{
  m_core.rollback_chain_to(height);
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_block_by_hash(const std::string& arg)
{
  Crypto::Hash block_hash;
  if (!parse_hash256(arg, block_hash)) {
    return false;
  }

  std::list<Crypto::Hash> block_ids;
  block_ids.push_back(block_hash);
  std::list<CryptoNote::Block> blocks;
  std::list<Crypto::Hash> missed_ids;
  m_core.get_blocks(block_ids, blocks, missed_ids);

  if (1 == blocks.size())
  {
    print_as_json(blocks.front());
  } else
  {
    std::cout << "block wasn't found: " << arg << std::endl;
    return false;
  }

  return true;
}
//--------------------------------------------------------------------------------
uint64_t DaemonCommandsHandler::calculatePercent(const CryptoNote::Currency& currency, uint64_t value, uint64_t total) {
  return static_cast<uint64_t>(100.0 * currency.coin() * static_cast<double>(value) / static_cast<double>(total));
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_stat(const std::vector<std::string>& args) {
  uint32_t height = 0;
  uint32_t maxHeight = m_core.getDaemonHeight() - 1;
  if (args.empty()) {
    height = maxHeight;
  } else {
    try {
      height = boost::lexical_cast<uint32_t>(args.front());
    } catch (boost::bad_lexical_cast&) {
      Crypto::Hash block_hash;
      if (!parse_hash256(args.front(), block_hash) || !m_core.getBlockHeight(block_hash, height)) {
        return false;
      }
    }
    if (height > maxHeight) {
      std::cout << "printing for last available block: " << maxHeight << std::endl;
      height = maxHeight;
    }
  }

  uint64_t totalCoinsInNetwork = m_core.coinsEmittedAtHeight(height);
  uint64_t totalCoinsOnDeposits = m_core.depositAmountAtHeight(height);
  uint64_t amountOfActiveCoins = totalCoinsInNetwork - totalCoinsOnDeposits;

  const auto& currency = m_core.currency();
  std::cout << "Block height: " << height << std::endl;
  std::cout << "Block difficulty: " << m_core.difficultyAtHeight(height) << std::endl;
  std::cout << "Total coins in network:  " << currency.formatAmount(totalCoinsInNetwork) << std::endl;
  std::cout << "Total coins banked: " << currency.formatAmount(totalCoinsOnDeposits) <<
    " (" << currency.formatAmount(calculatePercent(currency, totalCoinsOnDeposits, totalCoinsInNetwork)) << "%)" << std::endl;
  std::cout << "Amount of active coins:  " << currency.formatAmount(amountOfActiveCoins) <<
    " (" << currency.formatAmount(calculatePercent(currency, amountOfActiveCoins, totalCoinsInNetwork)) << "%)" << std::endl;
 
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_block(const std::vector<std::string> &args) {
  if (args.empty()) {
    std::cout << "expected: print_block (<block_hash> | <block_height>)" << std::endl;
    return true;
  }

  const std::string &arg = args.front();
  try {
    uint32_t height = boost::lexical_cast<uint32_t>(arg);
    print_block_by_height(height);
  } catch (boost::bad_lexical_cast &) {
    print_block_by_hash(arg);
  }

  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_tx(const std::vector<std::string>& args)
{
  if (args.empty()) {
    std::cout << "expected: print_tx <transaction hash>" << std::endl;
    return true;
  }

  const std::string &str_hash = args.front();
  Crypto::Hash tx_hash;
  if (!parse_hash256(str_hash, tx_hash)) {
    return true;
  }

  std::vector<Crypto::Hash> tx_ids;
  tx_ids.push_back(tx_hash);
  std::list<CryptoNote::Transaction> txs;
  std::list<Crypto::Hash> missed_ids;
  m_core.getTransactions(tx_ids, txs, missed_ids, true);

  if (1 == txs.size()) {
    print_as_json(txs.front());
  } else {
    std::cout << "transaction wasn't found: <" << str_hash << '>' << std::endl;
  }

  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_pool(const std::vector<std::string>& args)
{
  logger(Logging::INFO) << "Pool state: " << ENDL << m_core.print_pool(false);
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::print_pool_sh(const std::vector<std::string>& args)
{
  logger(Logging::INFO) << "Pool state: " << ENDL << m_core.print_pool(true);
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::start_mining(const std::vector<std::string> &args) {
  if (!args.size()) {
    std::cout << "Please, specify wallet address to mine for: start_mining <addr> [threads=1]" << std::endl;
    return true;
  }

  CryptoNote::AccountPublicAddress adr;
  if (!m_core.currency().parseAccountAddressString(args.front(), adr)) {
    std::cout << "target account address has wrong format" << std::endl;
    return true;
  }

  uint64_t threads_count = 1;
  if (args.size() > 1) {
    bool ok = Common::fromString(args[1], threads_count);
    threads_count = (ok && 0 < threads_count) ? threads_count : 1;
  }

  m_core.get_miner().start(adr, threads_count);
  return true;
}

//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::stop_mining(const std::vector<std::string>& args) {
  m_core.get_miner().stop();
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::showHelpTable() {
  Table helpTable;
  /* helpTable items */
  helpTable.add_row({"\"help\"", "Shows the Basic Commands Descriptions - This menu."});
  helpTable.add_row({"\"advanced\"", "Shows the Advanced Commands Descriptions."});
  helpTable.add_row({"\"help-usage\"", "Shows the Basic Commands Usage Guide."});
  helpTable.add_row({"\"advanced-usage\"", "Shows the Advanced Usage Guide."});

  helpTable.add_row({"\"exit\"", "Exits the Daemon safely."});
  helpTable.add_row({"\"set_log\"", "Changes the log level of the Daemon."});
  helpTable.add_row({"\"status\"", "Shows the current status of the Daemon."});
  // Maybe split this into a new help command, mining-help? 
  helpTable.add_row({"\"start_mining\"", "Starts mining with the Daemon to a certain address."});
  helpTable.add_row({"\"stop_mining\"", "Stops the miner that you started."});
  helpTable.add_row({"\"show_hr\"", "Shows the hashrate of your current miner."});
  helpTable.add_row({"\"hide_hr\"", "Hides the hashrate of your current miner."});

  /* format helpTable */
  helpTable.column(0).format().font_align(FontAlign::left).font_color(Color::green);
  helpTable.column(1).format().font_align(FontAlign::center).font_color(Color::magenta);
  helpTable.format().corner_color(Color::grey).border_color(Color::grey);

  std::cout << helpTable << std::endl;
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::showAdvancedTable() {
  Table advTable;
  /* advTable items */
  advTable.add_row({"\"rollback_chain\"", "Rollback the Blockchain to specific height."});
  advTable.add_row({"\"print_cn\"", "Shows the known connections."});
  advTable.add_row({"\"print_pl\"", "Shows the peer list."});
  advTable.add_row({"\"print_bc\"", "Shows the Blockchains information in a given height range."});
  advTable.add_row({"\"print_block\"", "Shows a blocks information."});
  advTable.add_row({"\"print_stat\"", "Shows statistics of a block."});
  advTable.add_row({"\"print_tx\"", "Print a transaction."});
  advTable.add_row({"\"print_pool\"", "Shows the current transaction pool (long format)."});
  advTable.add_row({"\"print_pool_sh\"", "Shows the current transaction pool (short format)."});

  /* format advTable */
  advTable.column(0).format().font_align(FontAlign::left).font_color(Color::green);
  advTable.column(1).format().font_align(FontAlign::center).font_color(Color::magenta);
  advTable.format().corner_color(Color::grey).border_color(Color::grey);

  std::cout << advTable << std::endl;
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::showHelpUsageTable() {
  Table helpUseTable;
  /* helpUseTable items */
  helpUseTable.add_row({"\"set_log\"", "\"set_log 3\"\nThis will set the log level at 3.\nUse numbers 1-4 when changing the log level. 1 = no logging and 4 = max logging."});
  helpUseTable.add_row({"\"start_mining\"", "\"start_mining ethiLfillYourAddressHere 4\"\nThis will start mining to the address \"ethiLfillYourAddressHere\" while using 4 threads."});

  /* format helpUseTable */
  helpUseTable.column(0).format().font_align(FontAlign::left).font_color(Color::green);
  helpUseTable.column(1).format().font_align(FontAlign::center).font_color(Color::magenta);
  helpUseTable.format().corner_color(Color::grey).border_color(Color::grey);

  std::cout << helpUseTable << std::endl;
  return true;
}
//--------------------------------------------------------------------------------
bool DaemonCommandsHandler::showAdvancedUsageTable() {
  Table advUseTable;
  /* advUseTable items */
  advUseTable.add_row({"\"rollback_chain\"", "\"rollback_chain 1\"\nThis will rollback the Blockchain to block 1.\n\"1\" = Block height."});
  advUseTable.add_row({"\"print_bc\"", "\"print_bc 1 10\"\nThis will show the Blockchain information for blocks 1 to 10.\n\"1\" = Start height. \"10\" = End height."});
  advUseTable.add_row({"\"print_block\"", "\"print_block as76db1298n7sna9f6afa8a5sd 1 4\"\nThis will show you block 1 in the Blockchain.\n\"as76db1298n7sna9f6afa8a5sd\" = Block hash. \"1\" = Block height."});
  advUseTable.add_row({"\"print_stat\"", "\"print_stat as76db1298n7sna9f6afa8a5sd 1\"\nThis will show you the block statistics.\n\"as76db1298n7sna9f6afa8a5sd\" = Block hash. \"1\" = Block height.\nYou can use \"print_stat\" on its own to show the last blocks statistics."});
  advUseTable.add_row({"\"print_tx\"", "\"print_tx tx76db1298n7sna9f6afa8a5sd12312zasd12csa\"\nThis will show you the transaction for \"tx76db1298n7sna9f6afa8a5sd12312zasd12csa\"\n\"tx76db1298n7sna9f6afa8a5sd12312zasd12csa\" = Transaction hash."});

  /* format advUseTable */
  advUseTable.column(0).format().font_align(FontAlign::left).font_color(Color::green);
  advUseTable.column(1).format().font_align(FontAlign::center).font_color(Color::magenta);
  advUseTable.format().corner_color(Color::grey).border_color(Color::grey);

  std::cout << advUseTable << std::endl;
  return true;
}
//--------------------------------------------------------------------------------