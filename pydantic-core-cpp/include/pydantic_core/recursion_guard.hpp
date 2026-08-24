#pragma once

#include <memory>
#include <unordered_set>
#include <cstdint>
#include <optional>

namespace pydantic_core {

// Recursion guard to prevent infinite loops in self-referencing schemas
// Matches Rust's RecursionState and RECURSION_GUARD_LIMIT
constexpr int RECURSION_GUARD_LIMIT = 100;

class RecursionGuard {
public:
    RecursionGuard() = default;
    
    // Check if we should recurse into this object
    // Returns false if we've hit the recursion limit or seen this object
    bool check_recurse(const void* obj_ptr) {
        if (depth_ >= RECURSION_GUARD_LIMIT) {
            return false;
        }
        
        // Check if we've seen this pointer before
        auto it = seen_objects_.find(obj_ptr);
        if (it != seen_objects_.end()) {
            return false;  // Already processing this object
        }
        
        seen_objects_.insert(obj_ptr);
        depth_++;
        return true;
    }
    
    // Mark that we're done processing this object
    void done_recurse(const void* obj_ptr) {
        seen_objects_.erase(obj_ptr);
        if (depth_ > 0) {
            depth_--;
        }
    }
    
    // Get current depth
    int depth() const { return depth_; }
    
    // Check if at limit
    bool at_limit() const { return depth_ >= RECURSION_GUARD_LIMIT; }
    
    // Reset state
    void reset() {
        seen_objects_.clear();
        depth_ = 0;
    }
    
private:
    std::unordered_set<const void*> seen_objects_;
    int depth_ = 0;
};

// Recursion state - holds the guard and provides RAII-style management
class RecursionState {
public:
    RecursionState() : guard_(new RecursionGuard()) {}
    
    // Start recursion into an object
    class RecursionEntry {
    public:
        RecursionEntry(RecursionGuard* guard, const void* obj)
            : guard_(guard), obj_(obj), allowed_(false) {}
        
        ~RecursionEntry() {
            if (allowed_) {
                guard_->done_recurse(obj_);
            }
        }
        
        bool allowed() const { return allowed_; }
        void set_allowed(bool v) { allowed_ = v; }
        
        // Move semantics
        RecursionEntry(RecursionEntry&& other) noexcept
            : guard_(other.guard_), obj_(other.obj_), allowed_(other.allowed_) {
            other.allowed_ = false;  // Don't decrement on destruction
        }
        
        RecursionEntry& operator=(RecursionEntry&& other) noexcept {
            if (allowed_) {
                guard_->done_recurse(obj_);
            }
            guard_ = other.guard_;
            obj_ = other.obj_;
            allowed_ = other.allowed_;
            other.allowed_ = false;
            return *this;
        }
        
    private:
        RecursionGuard* guard_;
        const void* obj_;
        bool allowed_;
    };
    
    // Enter recursion for an object
    RecursionEntry enter(const void* obj) {
        RecursionEntry entry(guard_.get(), obj);
        entry.set_allowed(guard_->check_recurse(obj));
        return entry;
    }
    
    // Get current depth
    int depth() const { return guard_->depth(); }
    
    // Check if at limit
    bool at_limit() const { return guard_->at_limit(); }
    
    // Reset
    void reset() { guard_->reset(); }
    
private:
    std::unique_ptr<RecursionGuard> guard_;
};

} // namespace pydantic_core