#include <math.h>

#include <eosiolib/eosio.hpp>
#include <eosiolib/transaction.hpp>
#include <eosiolib/asset.hpp>
#include <eosiolib/symbol.hpp>
#include <eosiolib/singleton.hpp>
#include <eosiolib/action.hpp>


using namespace eosio;
using std::string;
using std::vector;


// this is to access a multi-index in an external whitelist contract
struct account_row {
   name         account;    //account name
   uint8_t      score;      //enables a score between 0 to 255
   string       metadata;   //json meta data
   time_point_sec   timestamp;

   uint64_t primary_key()const { return account.value; }
   uint64_t by_timestamp() const { return uint64_t(timestamp.sec_since_epoch()); }
};
typedef eosio::multi_index<
  "accounts"_n, account_row,
  indexed_by<"bytime"_n, const_mem_fun<account_row, uint64_t, &account_row::by_timestamp>>
> accounts_score_table;


inline uint128_t ordbycycle(uint64_t cycle_number, name account) {
  return (((uint128_t)cycle_number)<<64) | account.value;
}

inline uint128_t ordbyuser(uint64_t cycle_number, name account) {
  return (((uint128_t)account.value)<<64) | cycle_number;
}



CONTRACT microauctions : public eosio::contract {
    using contract::contract;
    public:

        TABLE settings {
            name            whitelist;
            uint64_t        cycles;
            uint64_t        seconds_per_cycle;
            uint64_t        start_ts;
            extended_asset  quota_per_cycle;
            extended_asset  accepted_token;
            uint16_t        payouts_per_payin;  // how many outbound transfers to trigger on each inbound
            uint16_t        payouts_delay_sec;  // defferred transaction delay in seconds
            uint16_t        payout_cycles_per_user; // how many cycles aggregated per transfer for a user
        };

        typedef eosio::singleton<"settings"_n, settings> settings_t;



        TABLE cycle {
          asset    total_payins;
          uint64_t number;
          uint64_t primary_key()const { return number; }
        };

        typedef eosio::multi_index<"cycle"_n, cycle> cycles_t;


        TABLE payment {
          uint64_t  id;
          uint64_t  cycle_number;
          name      account;
          asset     quantity;
          uint64_t  primary_key()const { return id; }
          uint128_t get_ordbycycle()const { return ordbycycle(cycle_number, account); }
          uint128_t get_ordbyuser()const { return ordbyuser(cycle_number, account); }
        };

        typedef eosio::multi_index<"payment"_n, payment,
          indexed_by<"ordbycycle"_n, const_mem_fun<payment, uint128_t, &payment::get_ordbycycle>>,
          indexed_by<"ordbyuser"_n, const_mem_fun<payment, uint128_t, &payment::get_ordbyuser>>
          > payments_t;



        ACTION init(settings setting){
          require_auth(_self);
          settings_t settings_table(_self, _self.value);
          eosio_assert(!settings_table.exists(),"already inited");
          settings_table.set(setting, _self);
        }


        // aggregate payments for a user
        ACTION claim(name payer){
          send_tokens_for_user(payer, true);
        }


        void send_tokens_for_user(name payer, bool do_asserts) {
          payments_t payments_table(_self, _self.value);
          settings_t settings_table(_self, _self.value);
          auto current_settings = settings_table.get();
          int64_t current_cycle = getCurrentCycle(current_settings);
          cycles_t cycles_table(_self, _self.value);

          double total_payouts = 0;
          uint16_t count = current_settings.payout_cycles_per_user;
          eosio_assert(count > 0, "payout_cycles_per_user cannot be zero");
          bool found = false;
          vector<uint64_t> processed_cycles;

          auto payidx = payments_table.get_index<"ordbyuser"_n>();
          auto payitr = payidx.lower_bound(ordbyuser(0, payer));

          while( count-- > 0 && payitr != payidx.end() && payitr->account == payer ) {
            auto cycle_entry = cycles_table.find(payitr->cycle_number);
            eosio_assert(cycle_entry != cycles_table.end(), "Cannot find cycle by number");
            found = true;
            processed_cycles.emplace_back(payitr->cycle_number);

            // our_payin * total_payout / total_payins
            double payout =
              (double)(payitr->quantity.amount * current_settings.quota_per_cycle.quantity.amount) /
              cycle_entry->total_payins.amount;
            total_payouts += payout;
            payitr = payidx.erase(payitr);
          }

          if( do_asserts ) {
            eosio_assert(found, "There is nothing to claim for this account");
          }

          extended_asset payout;
          payout.contract = current_settings.quota_per_cycle.contract;
          payout.quantity.amount = total_payouts;
          payout.quantity.symbol = current_settings.quota_per_cycle.quantity.symbol;

          if( payout.quantity.amount > 0 ) {
            issueToken(payer, payout);
          }

          action {
            permission_level{_self, "active"_n},
            _self,
            "receipt"_n,
            receipt_abi {.payer=payer, .cycles=processed_cycles, .payout=payout}
          }.send();
        }



        // Send up to this many transfers. Anyone can trigger this action.
        ACTION sendtokens(uint16_t count) {
          payments_t payments_table(_self, _self.value);
          settings_t settings_table(_self, _self.value);
          auto current_settings = settings_table.get();
          uint64_t current_cycle = getCurrentCycle(current_settings);
          cycles_t cycles_table(_self, _self.value);

          auto payidx = payments_table.get_index<"ordbycycle"_n>();
          auto payitr = payidx.begin();
          while( count-- > 0 && payitr != payidx.end() && payitr->cycle_number < current_cycle ) {
            send_tokens_for_user(payitr->account, false);
            payitr = payidx.begin();
          }
        }



        // inline notifications
        struct receipt_abi {
          name payer;
          vector<uint64_t> cycles;
          extended_asset payout;
        };
        ACTION receipt(name payer, vector<uint64_t> cycles, extended_asset payout) {
          require_auth(_self);
          require_recipient(payer);
        }


        void transfer(name from, name to, asset quantity, string memo) {
          if(to != _self)
            return;
          require_auth(from);
          settings_t settings_table(_self, _self.value);
          auto current_settings = settings_table.get();

          // calculate current cycle
          uint64_t current_cycle = getCurrentCycle(current_settings);
          eosio_assert(current_cycle < current_settings.cycles, "auction ended");

          eosio_assert(quantity.symbol == current_settings.accepted_token.quantity.symbol,
                       "wrong asset symbol");
          eosio_assert(quantity.amount >= current_settings.accepted_token.quantity.amount,
                       "below minimum amount");
          eosio_assert(_code == current_settings.accepted_token.contract, "wrong asset contract");

          // parse memo to support different account than the sending account
          if (memo.size() > 0){
              name to_act = name(memo.c_str());
              eosio_assert(is_account(to_act), "The account name supplied is not valid");
              from = to_act;
          }
          eosio_assert(isWhitelisted(from), "whitelisting required");
          registerPayment(current_settings, current_cycle, from, quantity);
        }


    private:

      uint64_t getCurrentCycle(settings& current_settings){
        eosio_assert(current_time() >= current_settings.start_ts, "auction did not start yet");
        auto elapsed_time = current_time() - current_settings.start_ts;
        return elapsed_time / (current_settings.seconds_per_cycle * 1000000 );
      }



      void registerPayment(settings& current_settings, uint64_t current_cycle, name payer, asset quantity){
        payments_t payments_table(_self, _self.value);
        cycles_t cycles_table(_self, _self.value);
        auto current_cycle_entry = cycles_table.find(current_cycle);
        if(current_cycle_entry == cycles_table.end()){
          cycles_table.emplace(_self, [&](auto &s) {
            s.number = current_cycle;
            s.total_payins = quantity;
          });
        }
        else{
          cycles_table.modify(current_cycle_entry, _self, [&](auto &s) {
            s.total_payins += quantity;
          });
        }

        auto payidx = payments_table.get_index<"ordbycycle"_n>();
        auto payitr = payidx.find(ordbycycle(current_cycle, payer));
        if( payitr == payidx.end() ) {
          payments_table.emplace(_self, [&](auto &p) {
            p.id = payments_table.available_primary_key();
            p.cycle_number = current_cycle;
            p.account = payer;
            p.quantity = quantity;
          });
        }
        else{
          eosio_assert(payitr->cycle_number == current_cycle && payitr->account == payer,
                       "Retrieved a wrong payment row");
          payments_table.modify(*payitr, _self, [&](auto &p) {
            p.quantity += quantity;
          });
        }

        if( current_settings.payouts_per_payin > 0 ) {
          // if there are any pending payouts, schedule a deferred tx
          payitr = payidx.begin();
          // no need to check for payidx.end() because we just inserted an entry
          if( payitr->cycle_number < current_cycle) {
            transaction tx;
            tx.actions.emplace_back(
                                    permission_level{_self, "active"_n},
                                    _self, "sendtokens"_n,
                                    std::make_tuple(current_settings.payouts_per_payin)
                                    );
            tx.delay_sec = current_settings.payouts_delay_sec;
            tx.send(payer.value, _self);
          }
        }
      }



      bool isWhitelisted(name payer){
        settings_t settings_table(_self, _self.value);
        auto current_settings = settings_table.get();
        auto whitelist = current_settings.whitelist;
        if(whitelist == _self)
          return true;
        accounts_score_table accounts_scores(current_settings.whitelist, current_settings.whitelist.value);
        auto existing = accounts_scores.find(payer.value);
        return (existing != accounts_scores.end() && existing->score > 70);
      }



      void issueToken(name to, extended_asset quantity){
        action(permission_level{_self, "active"_n},
           quantity.contract, "issue"_n,
           std::make_tuple(to, quantity.quantity, std::string("DAPP token auction")))
        .send();
      }
};

extern "C" {
  [[noreturn]] void apply(uint64_t receiver, uint64_t code, uint64_t action) {
    if (action == "transfer"_n.value && code != receiver) {
      eosio::execute_action(eosio::name(receiver), eosio::name(code),
                            &microauctions::transfer);
    }
    if (code == receiver) {
      switch (action) {
        EOSIO_DISPATCH_HELPER(microauctions, (init)(sendtokens)(claim)(receipt))
      }
    }
    eosio_exit(0);
  }
}
