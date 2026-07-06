#include <iostream>
#include <memory>
#include <unordered_map>
#include <map>
#include <cstdint> 
#include <chrono>  

#include <vector>
#include <random>
#include <iomanip>

// Enum class for type safety
enum class OrderType {
    LIMIT,
    MARKET
};
enum class Side {
    BUY,
    SELL
};

struct Order {
    uint64_t orderId;       // Unique identifier
    uint32_t price;         // Stored as an integer (e.g., in cents/ticks)
    uint32_t quantity;      // Number of units
    Side side;              // BUY or SELL
    OrderType type;         // LIMIT or MARKWT
    uint64_t timestamp;     // Nanosecond timestamp for priority

    // Constructor
    Order(uint64_t id, uint32_t p, uint32_t q, Side s, OrderType t) : orderId(id), price(p), quantity(q), side(s), type(t) {
        // Automatically grab a high-resolution timestamp upon creation
        timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch()
                    ).count();
    }
};


struct OrderNode {
    std::shared_ptr<Order> order;         // pointer to the order
    std::shared_ptr<OrderNode> next;      // shared pointer to the next node
    std::weak_ptr<OrderNode> prev;        // Observes the previous node (prevents cyclic leaks)

    // Constructor
    OrderNode(std::shared_ptr<Order> o) : order(o) {}
};


struct LimitLevel {
    uint32_t price;
    uint32_t totalVolume;                 // Total quantity of shares at this price

    std::shared_ptr<OrderNode> head;      // First order in line (gets matched first)
    std::shared_ptr<OrderNode> tail;      // Last order in line (new orders go here)

    LimitLevel(uint32_t p) : price(p), totalVolume(0), head(nullptr), tail(nullptr) {}
};



class OrderBook {
private:
    // (O(1) lookups by Order ID)
    // Maps the OrderID (uint64_t) to a pointer to the exact OrderNode containing that Order
    std::unordered_map<uint64_t, std::shared_ptr<OrderNode>> orderMap;

    // (O(log N) sorted lookups by Price)
    // Bids (Buyers): We want the highest price first. 
    // std::greater for the tree to sort descending.
    std::map<uint32_t, std::shared_ptr<LimitLevel>, std::greater<uint32_t>> bids; 
    
    // Asks (Sellers): We want the lowest price first. 
    // Default std::map sorts ascending natively.
    std::map<uint32_t, std::shared_ptr<LimitLevel>> asks;

    // Helper function to handle the O(1) Doubly Linked List insertion
    void appendToLevel(std::shared_ptr<LimitLevel> level, std::shared_ptr<OrderNode> node) {
        
        // Scenario A: The price level is completely empty
        if (level->head == nullptr) {
            level->head = node;
            level->tail = node;
        } 
        // Scenario B: There are already orders of this price -> append this orderNode at the end
        else {
            level->tail->next = node;      // Old last node points forward to the new node
            node->prev = level->tail;      // New node points backward to the old last node
            level->tail = node;            // The new node is officially the new tail
        }

        // Update the total volume available at this price
        level->totalVolume += node->order->quantity;
    }

    void removeNodeFromLevel(std::shared_ptr<LimitLevel> level, std::shared_ptr<OrderNode> node) {
        // Step 1: Update the surrounding nodes
        if (auto p = node->prev.lock()) { // safely convert weak_ptr to shared_ptr
            p->next = node->next;
        } else {
            // If there is no previous node, this was the head.
            level->head = node->next;
        }

        if (node->next) {
            node->next->prev = node->prev;
        } else {
            // If there is no next node, this was the tail.
            level->tail = node->prev.lock();
        }

        // Step 2: Update total track volume
        level->totalVolume -= node->order->quantity;
    }

    // Returns the remaining quantity of the order that couldn't be filled
    uint32_t matchBuyOrder(uint32_t incomingPrice, uint32_t incomingQty) {
        
        auto it = asks.begin(); // Start at the absolute cheapest seller

        while (it != asks.end() && incomingQty > 0) {
            uint32_t askPrice = it->first;
            std::shared_ptr<LimitLevel> level = it->second;

            // If the cheapest seller is too expensive, stop matching immediately
            if (askPrice > incomingPrice) break;

            // Walk down the price level matching against resting orders
            std::shared_ptr<OrderNode> restingNode = level->head;
            
            while (restingNode != nullptr && incomingQty > 0) {
                // Determine trade size (minimum of what I want vs what they have)
                uint32_t tradeQty = std::min(incomingQty, restingNode->order->quantity);
                //std::cout << ">> EXECUTED TRADE: " << tradeQty << " shares @ " << askPrice << "\n";
                // Deduct the traded amount
                incomingQty -= tradeQty;
                restingNode->order->quantity -= tradeQty;
                level->totalVolume -= tradeQty;
                // If the resting order is completely filled, destroy it
                if (restingNode->order->quantity == 0) {
                    std::shared_ptr<OrderNode> nodeToDelete = restingNode;
                    restingNode = restingNode->next; // Move to the next node before destroying this one
                    removeNodeFromLevel(level, nodeToDelete);
                    orderMap.erase(nodeToDelete->order->orderId); // Hash map erase triggers memory cleanup
                } else {
                    // Resting order still has shares, but my incoming order is empty.
                    break; 
                }
            }

            // If we bought every single share at this price, destroy the entire price level
            if (level->head == nullptr) {
                it = asks.erase(it); // Erase from the Red-Black tree and move to the next price
            }
        }

        return incomingQty; // Return whatever didn't get filled
    }


    uint32_t matchSellOrder(uint32_t incomingPrice, uint32_t incomingQty) {
        
        auto it = bids.begin(); // Start at the absolute highest bidder

        while (it != bids.end() && incomingQty > 0) {
            uint32_t bidPrice = it->first;
            std::shared_ptr<LimitLevel> level = it->second;

            // If the highest bidder is too cheap, stop matching immediately
            if (bidPrice < incomingPrice) break;

            // Walk down the price level matching against resting orders
            std::shared_ptr<OrderNode> restingNode = level->head;
            
            while (restingNode != nullptr && incomingQty > 0) {
                // Determine trade size (minimum of what I want vs what they have)
                uint32_t tradeQty = std::min(incomingQty, restingNode->order->quantity);
                //std::cout << ">> EXECUTED TRADE: " << tradeQty << " shares @ " << bidPrice << "\n";
                // Deduct the traded amount
                incomingQty -= tradeQty;
                restingNode->order->quantity -= tradeQty;
                level->totalVolume -= tradeQty;
                // If the resting order is completely filled, destroy it
                if (restingNode->order->quantity == 0) {
                    std::shared_ptr<OrderNode> nodeToDelete = restingNode;
                    restingNode = restingNode->next; // Move to the next node before destroying this one
                    removeNodeFromLevel(level, nodeToDelete);
                    orderMap.erase(nodeToDelete->order->orderId); // Hash map erase triggers memory cleanup
                } else {
                    // Resting order still has shares, but my incoming order is empty.
                    break; 
                }
            }

            // If we bought every single share at this price, destroy the entire price lvel
            if (level->head == nullptr) {
                it = bids.erase(it); // Erase from the Red-Black tree and move to the next price
            }
        }

        return incomingQty; // Return whatever didn't get filled
    }


    void executeMarketBuy(uint64_t orderId, uint32_t incomingQty) {
        auto it = asks.begin(); // Start at the absolute cheapest seller

        while (it != asks.end() && incomingQty > 0) {
            uint32_t askPrice = it->first;
            std::shared_ptr<LimitLevel> level = it->second;

            std::shared_ptr<OrderNode> restingNode = level->head;
            
            while (restingNode != nullptr && incomingQty > 0) {
                uint32_t tradeQty = std::min(incomingQty, restingNode->order->quantity);

                //std::cout << ">> MARKET MATCH [Order " << orderId << "]: " << tradeQty << " shares @ " << askPrice << "\n";

                incomingQty -= tradeQty;
                restingNode->order->quantity -= tradeQty;
                level->totalVolume -= tradeQty;

                if (restingNode->order->quantity == 0) {
                    std::shared_ptr<OrderNode> nodeToDelete = restingNode;
                    restingNode = restingNode->next;
                    
                    removeNodeFromLevel(level, nodeToDelete);
                    orderMap.erase(nodeToDelete->order->orderId); 
                } else {
                    break; 
                }
            }

            if (level->head == nullptr) {
                it = asks.erase(it); // Erase empty price level and move to the next, more expensive track
            }
        }

        // Liquidity Dry-Up Handling
        if (incomingQty > 0) {
            //std::cout << "WARNING: Market order " << orderId << " partially filled. Remaining " << incomingQty << " shares canceled due to lack of liquidity.\n";
        } else {
            //std::cout << "Market order " << orderId << " fully filled.\n";
        }
    }


    void executeMarketSell(uint64_t orderId, uint32_t incomingQty) {
        auto it = bids.begin(); // Start at the absolute highest bidder

        while (it != bids.end() && incomingQty > 0) {
            uint32_t bidPrice = it->first;
            std::shared_ptr<LimitLevel> level = it->second;

            // A market order buys at ANY price until filled

            std::shared_ptr<OrderNode> restingNode = level->head;
            
            while (restingNode != nullptr && incomingQty > 0) {
                uint32_t tradeQty = std::min(incomingQty, restingNode->order->quantity);

                //std::cout << ">> MARKET MATCH [Order " << orderId << "]: " << tradeQty << " shares @ " << bidPrice << "\n";

                incomingQty -= tradeQty;
                restingNode->order->quantity -= tradeQty;
                level->totalVolume -= tradeQty;

                if (restingNode->order->quantity == 0) {
                    std::shared_ptr<OrderNode> nodeToDelete = restingNode;
                    restingNode = restingNode->next;
                    
                    removeNodeFromLevel(level, nodeToDelete);
                    orderMap.erase(nodeToDelete->order->orderId); 
                } else {
                    break; 
                }
            }

            if (level->head == nullptr) {
                it = bids.erase(it); // Erase empty price level and move to the next, less expensive level
            }
        }

        // Liquidity Dry-Up Handling
        if (incomingQty > 0) {
            //std::cout << "WARNING: Market order " << orderId << " partially filled. Remaining " << incomingQty << " shares canceled due to lack of liquidity.\n";
        } else {
            //std::cout << "Market order " << orderId << " fully filled.\n";
        }
    }


    void addOrder(uint64_t orderId, uint32_t price, uint32_t quantity, Side side) {
        
        // Step 1: Build the Order and OrderNode on the Heap
        auto newOrder = std::make_shared<Order>(orderId, price, quantity, side, OrderType::LIMIT);
        auto newNode = std::make_shared<OrderNode>(newOrder);

        // Step 2: Register the OrderNode for instant lookup (O(1) Hash Map)
        orderMap[orderId] = newNode;

        // Step 3: Route to the correct side (Bids or Asks)
        if (side == Side::BUY) {
            // If this price track doesn't exist yet, build it (O(log N))
            if (bids.find(price) == bids.end()) {
                bids[price] = std::make_shared<LimitLevel>(price);
            }
            // Couple the node to the level (O(1))
            appendToLevel(bids[price], newNode);
            
            //std::cout << "Added BUY  Order " << orderId << " for " << quantity << " shares @ " << price << "\n";
        } 
        else { // Side::SELL
            // If this price track doesn't exist yet, build it (O(log N))
            if (asks.find(price) == asks.end()) {
                asks[price] = std::make_shared<LimitLevel>(price);
            }
            // Couple the node to the level (O(1))
            appendToLevel(asks[price], newNode);
            
            //std::cout << "Added SELL Order " << orderId << " for " << quantity << " shares @ " << price << "\n";
        }
    }

public:
    // Demonstrating the O(1) Cancellation Logic
    void cancelOrder(uint64_t orderId) {
        
        // Step 1: Lookup the OrderID in the Hash Map
        auto it = orderMap.find(orderId);
        
        // If the iterator hits the end, the order doesn't exist
        if (it == orderMap.end()) {
            //std::cout << "Order " << orderId << " not found in the book.\n";
            return;
        }

        // Step 2: Grab the pointer to the OrderNode
        std::shared_ptr<OrderNode> nodeToCancel = it->second;

        //std::cout << "Locating Order " << orderId << " at price level " << nodeToCancel->order->price << "...\n";

        // Step 3: Uncouple the node from the price level
        uint32_t orderPrice = nodeToCancel->order->price;
        std::shared_ptr<LimitLevel> parentLevel;

        if (nodeToCancel->order->side == Side::BUY) {
            parentLevel = bids[orderPrice];
        } else {
            parentLevel = asks[orderPrice];
        }

        // Now we pass the parentLevel and the node to the helper function we wrote up
        removeNodeFromLevel(parentLevel, nodeToCancel);

        if (parentLevel->head == nullptr) {//if the level becomes empty by cancelling this order, remove the level from the respective map
            if (nodeToCancel->order->side == Side::BUY) {
                bids.erase(orderPrice);
            } else {
                asks.erase(orderPrice);
            }
        }
        
        // Step 4: Delete the container from the hash map
        orderMap.erase(it);

        //std::cout << "Order " << orderId << " successfully canceled and memory freed.\n";
    }


    void processLimitOrder(uint64_t orderId, uint32_t price, uint32_t quantity, Side side) {
        uint32_t remainingQty = quantity;

        if (side == Side::BUY) {
            remainingQty = matchBuyOrder(price, remainingQty);
        } else {
            remainingQty = matchSellOrder(price, remainingQty); // Same logic, just inverted
        }

        // If the order wasn't fully filled by the matching engine, add the rest to the book
        if (remainingQty > 0) {
            addOrder(orderId, price, remainingQty, side);
        }
    }
    
    
    void processMarketOrder(uint64_t orderId, uint32_t quantity, Side side){
        //std::cout << "Processing Market " << (side == Side::BUY ? "BUY" : "SELL") << " Order " << orderId << " for " << quantity << " shares.\n";
        if (side == Side::BUY) {
            executeMarketBuy(orderId, quantity);
        } else {
            executeMarketSell(orderId, quantity);
        }
    }
};


//******************************************************************************************************//
// TESTING LOGIC


// A struct to hold our pre-generated test data
struct TestOrder {
    uint64_t orderId;
    uint32_t price;
    uint32_t quantity;
    Side side;
    OrderType type;
    bool isCancel; // Flag to randomly test the O(1) cancellation
};

int main() {
    OrderBook engine;
    
    // 1. Configuration
    const int NUM_ORDERS = 1000000; // 1 Million orders
    std::cout << "Preparing to benchmark engine with " << NUM_ORDERS << " orders...\n";

    std::vector<TestOrder> orderStream;
    orderStream.reserve(NUM_ORDERS);

    // 2. Realistic Market Simulation Setup
    std::random_device rd;
    std::mt19937 gen(rd());
    
    // Prices clustered around $150.00 (15000 cents) to force heavy matching
    std::normal_distribution<> priceDist(15000, 50); 
    std::uniform_int_distribution<> qtyDist(10, 500);
    std::uniform_int_distribution<> sideDist(0, 1);
    std::uniform_int_distribution<> typeDist(1, 100); 
    std::uniform_int_distribution<> cancelDist(1, 100);

    // 3. Pre-generate the data to avoid skewing the timer
    std::vector<uint64_t> activeIds; // Keep track of IDs so we can cancel valid ones
    activeIds.reserve(NUM_ORDERS);

    for (uint64_t i = 1; i <= NUM_ORDERS; ++i) {
        TestOrder to;
        to.orderId = i;
        to.price = std::max(1, static_cast<int>(priceDist(gen))); // Prevent negative prices
        to.quantity = qtyDist(gen);
        to.side = (sideDist(gen) == 0) ? Side::BUY : Side::SELL;
        
        // 5% Market Orders, 95% Limit Orders
        to.type = (typeDist(gen) <= 5) ? OrderType::MARKET : OrderType::LIMIT;
        
        // 10% chance to be a cancellation request (only if we have active orders)
        to.isCancel = (cancelDist(gen) <= 10 && !activeIds.empty());

        if (to.isCancel) {
            // Pick a random resting order to cancel
            std::uniform_int_distribution<> idPicker(0, activeIds.size() - 1);
            to.orderId = activeIds[idPicker(gen)];
        } else if (to.type == OrderType::LIMIT) {
            activeIds.push_back(to.orderId);
        }

        orderStream.push_back(to);
    }

    std::cout << "Data generation complete. Starting stress test...\n";

    // 4. The Timed Execution (The true benchmark)
    auto startTime = std::chrono::high_resolution_clock::now();

    for (const auto& order : orderStream) {
        if (order.isCancel) {
            engine.cancelOrder(order.orderId);
        } else if (order.type == OrderType::LIMIT) {
            engine.processLimitOrder(order.orderId, order.price, order.quantity, order.side);
        } else {
            engine.processMarketOrder(order.orderId, order.quantity, order.side);
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();

    // 5. Metric Calculations
    auto durationNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();
    double durationSeconds = durationNanos / 1e9;
    
    double throughput = NUM_ORDERS / durationSeconds;
    double avgLatencyMicros = (durationNanos / 1000.0) / NUM_ORDERS;

    // 6. Output the Results
    std::cout << "\n========================================\n";
    std::cout << "        BENCHMARK RESULTS               \n";
    std::cout << "========================================\n";
    std::cout << "Total Orders Processed : " << NUM_ORDERS << "\n";
    std::cout << "Total Execution Time   : " << std::fixed << std::setprecision(4) << durationSeconds << " seconds\n";
    std::cout << "Throughput             : " << std::fixed << std::setprecision(0) << throughput << " orders/sec\n";
    std::cout << "Average Latency        : " << std::fixed << std::setprecision(3) << avgLatencyMicros << " microseconds/order\n";
    std::cout << "========================================\n";

    return 0;
}
