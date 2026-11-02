#pragma once

#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QString>

#include <algorithm>
#include <cmath>

namespace net {

// What one provider is asked per second, by everything GitWallet does. A token
// bucket: `perSecond` calls, with bursts up to two seconds' worth. A request larger
// than the bucket waits until it is full, then goes, and the ones after it wait off
// the rest of what it cost. GUI thread only.
class Budget
{
public:
    explicit Budget(int perSecond) : perSecond_(perSecond), tokens_(perSecond * 2.0) { clock_.start(); }

    // How long `calls` more have to wait, in ms; 0: they may go now, and they count.
    int take(int calls)
    {
        refill();
        const double needed = std::min(double(calls), perSecond_ * 2.0);
        if (tokens_ >= needed) {
            tokens_ -= calls;
            return 0;
        }
        return std::max(1, int(std::ceil((needed - tokens_) * 1000.0 / perSecond_)));
    }

    void lowerPace(int perSecond)
    {
        if (perSecond >= perSecond_) return;
        perSecond_ = perSecond;
        tokens_ = std::min(tokens_, perSecond_ * 2.0);
    }

private:
    void refill()
    {
        const qint64 ms = clock_.restart();
        tokens_ = std::min(perSecond_ * 2.0, tokens_ + ms * perSecond_ / 1000.0);
    }

    double perSecond_;
    double tokens_;
    QElapsedTimer clock_;
};

// One budget per host, shared by every client that talks to it, at the lowest pace any
// of them knows for it.
class BudgetPool
{
public:
    Budget& forHost(const QString& host, int perSecond)
    {
        const auto found = budgets_.find(host);
        if (found != budgets_.end()) {
            found->lowerPace(perSecond);
            return *found;
        }
        return *budgets_.emplace(host, perSecond);
    }

private:
    QHash<QString, Budget> budgets_;
};

} // namespace net
