// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2016-2018, The Karbo developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 Conceal Network & Conceal Devs
// Copyright (c) 2019-2020 The Lithe Project Development Team

// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "version.h"

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "DaemonCommandsHandler.h"

#include "Common/SignalHandler.h"
#include "Common/PathTools.h"
#include "crypto/hash.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/CoreConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/MinerConfig.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "P2p/NetNode.h"
#include "P2p/NetNodeConfig.h"
#include "Rpc/RpcServer.h"
#include "Rpc/RpcServerConfig.h"
#include "version.h"

#include "Common/ColouredMsg.h"
#include "Logging/ConsoleLogger.h"
#include <Logging/LoggerManager.h>

#include <algorithm>

using Common::JsonValue;
using namespace CryptoNote;
using namespace Logging;

namespace po = boost::program_options;

namespace
{
  const command_line::arg_descriptor<std::string> arg_config_file = {"config-file", "Specify configuration file", std::string(CryptoNote::CRYPTONOTE_NAME) + ".conf"};
  const command_line::arg_descriptor<bool>        arg_os_version  = {"os-version", ""};
  const command_line::arg_descriptor<std::string> arg_log_file    = {"log-file", "", ""};
  const command_line::arg_descriptor<std::string> arg_set_fee_address = { "fee-address", "Set a fee address for remote nodes", "" };
  const command_line::arg_descriptor<std::string> arg_set_view_key = { "view-key", "Set secret view-key for remote node fee confirmation", "" };
  const command_line::arg_descriptor<int>         arg_log_level   = {"log-level", "", 2}; // info level
  const command_line::arg_descriptor<bool>        arg_console     = {"no-console", "Disable daemon console commands"};
  const command_line::arg_descriptor<bool>        arg_testnet_on  = {"testnet", "Used to deploy test nets. Checkpoints and hardcoded seeds are ignored, "
    "network id is changed. Use it with --data-dir flag. The wallet must be launched with --testnet flag.", false};
  const command_line::arg_descriptor<bool>        arg_print_genesis_tx = { "print-genesis-tx", "Prints genesis' block tx hex to insert it to config and exits" };
  const command_line::arg_descriptor<std::vector<std::string>> arg_enable_cors = { "enable-cors", "Adds header 'Access-Control-Allow-Origin' to the daemon's RPC responses. Uses the value as domain. Use * for all" };
  const command_line::arg_descriptor<bool>        arg_blockexplorer_on = {"enable-blockexplorer", "Enable blockchain explorer RPC", false};
}

bool command_line_preprocessor(const boost::program_options::variables_map& vm, LoggerRef& logger);

void print_genesis_tx_hex() {
  Logging::ConsoleLogger logger;
  CryptoNote::Transaction tx = CryptoNote::CurrencyBuilder(logger).generateGenesisTransaction();
  CryptoNote::BinaryArray txb = CryptoNote::toBinaryArray(tx);
  std::string tx_hex = Common::toHex(txb);

  std::cout << "Insert this line into your coin configuration file as is: " << std::endl;
  std::cout << "const char GENESIS_COINBASE_TX_HEX[] = \"" << tx_hex << "\";" << std::endl;

  return;
}

JsonValue buildLoggerConfiguration(Level level, const std::string& logfile) {
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue& cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue& fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  JsonValue& consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "%T %L ");

  return loggerConfiguration;
}

int main(int argc, char* argv[]) {
  LoggerManager logManager;
  LoggerRef logger(logManager, "daemon");

  try {
    po::options_description desc_cmd_only("Command line options");
    po::options_description desc_cmd_sett("Command line options and settings options");

    command_line::add_arg(desc_cmd_only, command_line::arg_help);
    command_line::add_arg(desc_cmd_only, command_line::arg_version);
    command_line::add_arg(desc_cmd_only, arg_os_version);
    command_line::add_arg(desc_cmd_only, command_line::arg_data_dir, Tools::getDefaultDataDirectory());
    command_line::add_arg(desc_cmd_only, arg_config_file);
	  command_line::add_arg(desc_cmd_sett, arg_set_fee_address);
    command_line::add_arg(desc_cmd_sett, arg_log_file);
    command_line::add_arg(desc_cmd_sett, arg_log_level);
    command_line::add_arg(desc_cmd_sett, arg_console);
  	command_line::add_arg(desc_cmd_sett, arg_set_view_key);
    command_line::add_arg(desc_cmd_sett, arg_testnet_on);
    command_line::add_arg(desc_cmd_sett, arg_print_genesis_tx);
    command_line::add_arg(desc_cmd_sett, arg_enable_cors);
    command_line::add_arg(desc_cmd_sett, arg_blockexplorer_on);

    RpcServerConfig::initOptions(desc_cmd_sett);
    CoreConfig::initOptions(desc_cmd_sett);
    NetNodeConfig::initOptions(desc_cmd_sett);
    MinerConfig::initOptions(desc_cmd_sett);

    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd_only).add(desc_cmd_sett);

    po::variables_map vm;
    bool r = command_line::handle_error_helper(desc_options, [&]()
    {
      po::store(po::parse_command_line(argc, argv, desc_options), vm);

      if (command_line::get_arg(vm, command_line::arg_help))
      {
        std::cout << CryptoNote::CRYPTONOTE_NAME << " v" << PROJECT_VERSION_LONG << ENDL << ENDL;
        std::cout << desc_options << std::endl;
        return false;
      }

      if (command_line::get_arg(vm, arg_print_genesis_tx)) {
        //print_genesis_tx_hex(vm);
		    print_genesis_tx_hex();
        return false;
      }

      std::string data_dir = command_line::get_arg(vm, command_line::arg_data_dir);
      std::string config = command_line::get_arg(vm, arg_config_file);

      boost::filesystem::path data_dir_path(data_dir);
      boost::filesystem::path config_path(config);
      if (!config_path.has_parent_path()) {
        config_path = data_dir_path / config_path;
      }

      boost::system::error_code ec;
      if (boost::filesystem::exists(config_path, ec)) {
        po::store(po::parse_config_file<char>(config_path.string<std::string>().c_str(), desc_cmd_sett), vm);
      }

      po::notify(vm);
      return true;
    });

    if (!r) {
      return 1;
    }

    auto modulePath = Common::NativePathToGeneric(argv[0]);
    auto cfgLogFile = Common::NativePathToGeneric(command_line::get_arg(vm, arg_log_file));

    if (cfgLogFile.empty()) {
      cfgLogFile = Common::ReplaceExtenstion(modulePath, ".log");
    } else {
      if (!Common::HasParentPath(cfgLogFile)) {
        cfgLogFile = Common::CombinePath(Common::GetPathDirectory(modulePath), cfgLogFile);
      }
    }

    Level cfgLogLevel = static_cast<Level>(static_cast<int>(Logging::ERROR) + command_line::get_arg(vm, arg_log_level));

    // configure logging
    logManager.configure(buildLoggerConfiguration(cfgLogLevel, cfgLogFile));

    /* write to the log what version this is */
    logger(DEBUGGING) << "Lithe v" << PROJECT_VERSION_LONG;
    /* now display it to the user. Displays "Lithe v0.0.2 - Pre-Alpha-Stage2" */
    std::cout << std::endl << MagentaMsg("Lithe") << BrightMagentaMsg("v" PROJECT_VERSION " - " PROJECT_VERSION_BUILD_NO);

    if (command_line_preprocessor(vm, logger)) {
      return 0;
    }

    /* show the module folder only within log file */
    logger(DEBUGGING) << "Module folder: " << argv[0];

    bool testnet_mode = command_line::get_arg(vm, arg_testnet_on);
    if (testnet_mode) {
      /* tell the log testnet is active */
      logger(DEBUGGING) << "Started the Daemon in testnet mode.";
      
      /* now tell the user */
      std::cout << std::endl << BrightYellowMsg("Activating Testnet") << std::endl
              << YellowMsg("You have started your daemon in Testnet mode")
              << std::endl 
              << std::endl << YellowMsg("Remember, coins generated in testnet are not real!")
              << std::endl << std::endl;
    }

    //create objects and link them
    CryptoNote::CurrencyBuilder currencyBuilder(logManager);
    currencyBuilder.testnet(testnet_mode);
    bool blockexplorer_mode = command_line::get_arg(vm, arg_blockexplorer_on);
    currencyBuilder.isBlockexplorer(blockexplorer_mode);

    try {
      currencyBuilder.currency();
    } catch (std::exception&) {
      std::cout << "WHOOPS! It looks like the genesis transaction hex has been changed.";
      return 1;
    }

    CryptoNote::Currency currency = currencyBuilder.currency();
    CryptoNote::core ccore(currency, nullptr, logManager);

     CoreConfig coreConfig;
    coreConfig.init(vm);
    NetNodeConfig netNodeConfig;
    netNodeConfig.init(vm);
    netNodeConfig.setTestnet(testnet_mode);
    MinerConfig minerConfig;
    minerConfig.init(vm);
    RpcServerConfig rpcConfig;
    rpcConfig.init(vm);

    if (!coreConfig.configFolderDefaulted) {
      if (!Tools::directoryExists(coreConfig.configFolder)) {
        throw std::runtime_error("Directory does not exist: " + coreConfig.configFolder);
      }
    } else {
      if (!Tools::create_directories_if_necessary(coreConfig.configFolder)) {
        throw std::runtime_error("Can't create directory: " + coreConfig.configFolder);
      }
    }

    System::Dispatcher dispatcher;

    CryptoNote::CryptoNoteProtocolHandler cprotocol(currency, dispatcher, ccore, nullptr, logManager);
    CryptoNote::NodeServer p2psrv(dispatcher, cprotocol, logManager);
    CryptoNote::RpcServer rpcServer(dispatcher, logManager, ccore, p2psrv, cprotocol);

    cprotocol.set_p2p_endpoint(&p2psrv);
    ccore.set_cryptonote_protocol(&cprotocol);

    // initialize objects
    /* tell the log */
    logger(DEBUGGING) << "Initializing p2p server...";
    /* now the user */
    std::cout << YellowMsg("Starting P2P Server...") << std::endl;

    if (!p2psrv.init(netNodeConfig)) {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize p2p server.";
      return 1;
    }
    
    /* tell the log */
    logger(DEBUGGING) << "P2p server initialized OK";
    /* now the user */
    std::cout << BrightGreenMsg("P2P Server is active.") << std::endl;

    // initialize core here
    /* tell the log */
    logger(DEBUGGING) << "Initializing core...";
    /* now the user */
    std::cout << YellowMsg("Starting Core...") << std::endl;

    if (!ccore.init(coreConfig, minerConfig, true)) {
      /* tell the user and the log */
      logger(ERROR, BRIGHT_RED) << "- Daemon.cpp - Failed to initialize core";
      return 1;
    }

    /* tell the log */
    logger(DEBUGGING) << "Core initialized OK";
    /* now the user */
    std::cout << BrightGreenMsg("Core is active.") << std::endl;

    /* tell the log */
    logger(DEBUGGING) << "Starting core rpc server on address " << rpcConfig.getBindAddress();
    /* now the user */
    std::cout << YellowMsg("Starting Core RPC Server...") << std::endl;

    /* Set address for remote node fee */
  	if (command_line::has_arg(vm, arg_set_fee_address)) {
	  std::string addr_str = command_line::get_arg(vm, arg_set_fee_address);
	  if (!addr_str.empty()) {
        AccountPublicAddress acc = boost::value_initialized<AccountPublicAddress>();
        if (!currency.parseAccountAddressString(addr_str, acc)) {
          /* tell the user and log file */
          logger(ERROR, BRIGHT_RED) << "Bad fee address: " << addr_str;
          return 1;
        }
        rpcServer.setFeeAddress(addr_str, acc);
        /* tell the log */
        logger(DEBUGGING) << "Remote node fee address set: " << addr_str;
        /* now the user */
        std::cout << BrightGreenMsg("Remote node address set to: ") << BrightMagentaMsg(addr_str) << std::endl;
      }
	  }
  
    /* This sets the view-key so we can confirm that
       the fee is part of the transaction blob */       
    if (command_line::has_arg(vm, arg_set_view_key)) {
      std::string vk_str = command_line::get_arg(vm, arg_set_view_key);
	    if (!vk_str.empty()) {
        rpcServer.setViewKey(vk_str);
        /* tell the log */
        logger(DEBUGGING) << "Secret view key set: " << vk_str;
        /* now the user */
        std::cout << BrightGreenMsg("Secret View Key set: ") << BrightMagentaMsg(vk_str) << std::endl;
      }
    }
 
    rpcServer.start(rpcConfig.bindIp, rpcConfig.bindPort);
	  rpcServer.enableCors(command_line::get_arg(vm, arg_enable_cors));
    /* tell the log */
    logger(DEBUGGING) << "Core rpc server started ok";
    /* now the user */
    std::cout << BrightGreenMsg("Core RPC Server started on: ") << BrightMagentaMsg(rpcConfig.getBindAddress()) << std::endl;

    DaemonCommandsHandler dch(ccore, p2psrv, logManager, &rpcServer);

    // start components
    if (!command_line::has_arg(vm, arg_console)) {
      dch.start_handling();
    }

    Tools::SignalHandler::install([&dch, &p2psrv] {
      dch.stop_handling();
      p2psrv.sendStopSignal();
    });

    /* tell the log */
    logger(DEBUGGING) << "Starting p2p net loop...";
    /* now the user */
    std::cout << BrightGreenMsg("Starting P2P Net Loop.") << std::endl;
    p2psrv.run();
    /* tell the log */
    logger(DEBUGGING) << "p2p net loop stopped";
    /* now the user */
    std::cout << GreenMsg("P2P Net Loop is now stopping.") << std::endl;

    dch.stop_handling();

    //stop components
    /* tell the log */
    logger(DEBUGGING) << "Stopping core rpc server...";
    /* now the user */
    std::cout << GreenMsg("Core RPC Server has now stopped.") << std::endl;
    rpcServer.stop();

    //deinitialize components
    /* tell the log */
    logger(DEBUGGING) << "Deinitializing core...";
    /* now the user */
    std::cout << GreenMsg("Core has now stopped.") << std::endl;
    ccore.deinit();
    /* tell the log */
    logger(DEBUGGING) << "Deinitializing p2p...";
    /* now the user */
    std::cout << GreenMsg("P2P Net Loop has now stopped.") << std::endl;
    p2psrv.deinit();

    ccore.set_cryptonote_protocol(NULL);
    cprotocol.set_p2p_endpoint(NULL);

  } catch (const std::exception& e) {
    /* tell the user and log file */
    logger(ERROR, BRIGHT_RED) << "Exception: " << e.what();
    return 1;
  }

  /* tell the log */
  logger(DEBUGGING) << "Node stopped.";
  /* now the user */
  std::cout << GreenMsg("The Daemon has now stopped.") << std::endl;

  return 0;
}

bool command_line_preprocessor(const boost::program_options::variables_map &vm, LoggerRef &logger) {
  bool exit = false;

  if (command_line::get_arg(vm, command_line::arg_version)) {
    std::cout << CryptoNote::CRYPTONOTE_NAME << " v" << PROJECT_VERSION_LONG << ENDL;
    exit = true;
  }

  if (command_line::get_arg(vm, arg_os_version)) {
    std::cout << "OS: " << Tools::get_os_version_string() << ENDL;
    exit = true;
  }

  if (exit) {
    return true;
  }

  return false;
}
