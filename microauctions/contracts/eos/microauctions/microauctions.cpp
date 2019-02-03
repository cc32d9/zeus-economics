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

TABLE account_row {
   name         account;    //account name
   uint8_t      score;      //enables a score between 0 to 255
   string       metadata;   //json meta data
   time_point_sec   timestamp;

   uint64_t primary_key()const { return account.value; }
   uint64_t by_timestamp() const { return uint64_t(timestamp.sec_since_epoch()); }
};
typedef eosio::multi_index<
  name("accounts"), account_row,
  indexed_by<name("bytime"), const_mem_fun<account_row, uint64_t, &account_row::by_timestamp>>
> accounts_score_table;


CONTRACT microauctions : public eosio::contract {
    using contract::contract;
    public:

        TABLE settings {
            name            whitelist;
            uint64_t        cycles;
            uint64_t        seconds_per_cycle;
            uint64_t        start_ts;
            extended_asset  quantity_per_day;
            extended_asset  accepted_token;
        };
        
        
        
        
        TABLE cycle {
          asset quantity;
          uint64_t number;
          uint64_t primary_key()const { return number; }
        };
        
        
        
        
        TABLE account {
          std::vector<cycle> amounts_cycles;
        };


        typedef eosio::singleton<"settings"_n, settings> settings_t;
        typedef eosio::singleton<"account"_n, account> accounts_t;
        typedef eosio::multi_index<"cycle"_n, cycle> cycles_t;
        
        
        ACTION init(settings setting){
          require_auth(_self);
          settings_t settings_table(_self, _self.value);
          // eosio_assert(!settings_table.exists(),"already inited");
          settings_table.set(setting, _self);
        }

        
        ACTION claim(name to){
          // anyone can claim for a user
          accounts_t accounts_table(_self, to.value);
          settings_t settings_table(_self, _self.value);
          cycles_t cycles_table(_self, _self.value);
          auto current_settings = settings_table.get();
          auto existing = accounts_table.get();
          uint64_t cycle_number = getCurrentCycle();
          
          // foreach: days before current cycle
          auto amounts_cycles = existing.amounts_cycles;
          auto quantity_per_day = current_settings.quantity_per_day;
          double total = 0;
          for (int i = amounts_cycles.size()-1; i >=0 ; i--) {
            auto current_cycle = amounts_cycles[i];
            if(cycle_number <= current_cycle.number){ 
              // cycle not complete
              continue; 
            }
            auto account_quantity = current_cycle.quantity;
            if(account_quantity.amount == 0)
              continue;
            auto cycle_entry = cycles_table.find(current_cycle.number);
            // calculate tokens
            double token_price = (double)cycle_entry->quantity.amount / quantity_per_day.quantity.amount;
            total += (double)account_quantity.amount / token_price;
            amounts_cycles.erase(amounts_cycles.begin() + i);
          }
          if(total >= 1){
            extended_asset tokens;
            tokens.contract = quantity_per_day.contract;
            tokens.quantity.amount = total;
            tokens.quantity.symbol = quantity_per_day.quantity.symbol;
            issueToken(to, tokens);
          }
          // remove all
          if(amounts_cycles.size() > 0){
            accounts_table.set(existing, _self);
          }
          else
            accounts_table.remove();
        }
        

        void transfer(name from, name to, asset quantity, string memo){
          if(from == _self)
            return;
          require_auth(from);
          accounts_t accounts_table(_self, from.value);
          settings_t settings_table(_self, _self.value);
          auto current_settings = settings_table.get();
          
          // calculate current cycle
          uint64_t cycle_number = getCurrentCycle();
          eosio_assert(cycle_number < current_settings.cycles, "auction ended");
          eosio_assert(isWhitelisted(from), "whitelisting required");
          eosio_assert(quantity.symbol == current_settings.accepted_token.quantity.symbol, "wrong asset symbol");
          eosio_assert(quantity.amount >= current_settings.accepted_token.quantity.amount, "below minimum amount");
          eosio_assert(_code == current_settings.accepted_token.contract, "wrong asset contract");
          
          // TODO: parse memo to support different account than the sending account
          increaseCycleAmountAccount(cycle_number, from, quantity);
        }
    private:
      uint64_t getCurrentCycle(){
        settings_t settings_table(_self, _self.value);
        auto current_settings = settings_table.get();
        auto elapsed_time = current_time() - current_settings.start_ts;
        return elapsed_time / (current_settings.seconds_per_cycle * 1000000 );
      }
      
      void increaseCycleAmountAccount(uint64_t cycle_number, name from, asset quantity){
        accounts_t accounts_table(_self, from.value);
        cycles_t cycles_table(_self, _self.value);
        auto current_cycle_entry = cycles_table.find(cycle_number);
        if(current_cycle_entry == cycles_table.end()){
          cycles_table.emplace(_self, [&](auto &s) {
            s.number = cycle_number;
            s.quantity = quantity;
          });
        }
        else{
          cycles_table.modify(current_cycle_entry, _self, [&](auto &s) {
            s.quantity += quantity;
          });
        }
        
        account current_account;
        if(accounts_table.exists())
          current_account = accounts_table.get();
        
        bool found = false;
        for (int i = 0; i < current_account.amounts_cycles.size(); i++) {
          if(current_account.amounts_cycles[i].number != cycle_number)
            continue;
          
          current_account.amounts_cycles[i].quantity += quantity;
          found = true;
          break;
        }
        if(!found){
            cycle amount_cycle;  
            amount_cycle.number = cycle_number;
            amount_cycle.quantity = quantity;
            current_account.amounts_cycles.insert(current_account.amounts_cycles.end(), amount_cycle);
        }
        accounts_table.set(current_account, eosio::same_payer);
        
      }
      bool isWhitelisted(name from){
        settings_t settings_table(_self, _self.value);
        auto current_settings = settings_table.get();
        auto whitelist = current_settings.whitelist;
        if(whitelist == _self)
          return true;
        accounts_score_table accounts_scores(current_settings.whitelist, current_settings.whitelist.value);
        auto existing = accounts_scores.find(from.value);
        if(existing != accounts_scores.end()){
          if(existing->score > 70 )
            return true;
        }
        
        return false;
        
        
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
        EOSIO_DISPATCH_HELPER(microauctions, (init)(claim))
      }
    }
    eosio_exit(0);
  }
}
