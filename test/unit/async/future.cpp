#include <util/async_suite.hpp>
#include <util/error_code.hpp>
#include <util/error_suite.hpp>
#include <util/intrusive_list.hpp>

#include <yaclib/algo/detail/inline_core.hpp>
#include <yaclib/algo/detail/result_core.hpp>
#include <yaclib/async/contract.hpp>
#include <yaclib/async/future.hpp>
#include <yaclib/async/make.hpp>
#include <yaclib/async/promise.hpp>
#include <yaclib/async/run.hpp>
#include <yaclib/async/wait.hpp>
#include <yaclib/exe/executor.hpp>
#include <yaclib/exe/job.hpp>
#include <yaclib/exe/manual.hpp>
#include <yaclib/exe/submit.hpp>
#include <yaclib/exe/thread_pool.hpp>
#include <yaclib/util/intrusive_ptr.hpp>
#include <yaclib/util/result.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <yaclib_std/thread>

#include <gtest/gtest.h>

namespace test {
namespace {

using namespace std::chrono_literals;

template <typename FutureType, typename E = yaclib::StopError, typename ErrorType>
void ErrorsCheck(ErrorType expected) {
  static_assert(std::is_same_v<ErrorType, std::exception_ptr> || std::is_same_v<ErrorType, yaclib::StopError> ||
                std::is_same_v<ErrorType, LikeErrorCode>);
  auto [f, p] = yaclib::MakeContract<FutureType, E>();
  EXPECT_FALSE(f.Ready());
  std::move(p).Set(expected);
  EXPECT_TRUE(f.Ready());
  auto result = std::move(f).Get();
  if constexpr (std::is_same_v<ErrorType, std::exception_ptr>) {
    EXPECT_EQ(result.State(), yaclib::ResultState::Exception);
    EXPECT_EQ(std::move(result).Exception(), expected);
  } else {
    EXPECT_EQ(result.State(), yaclib::ResultState::Error);
    EXPECT_EQ(std::move(result).Error(), expected);
  }
}

// FutureType must be default constructible
template <typename FutureType>
void ValueCheck() {
  auto [f, p] = yaclib::MakeContract<FutureType>();
  using Value = std::conditional_t<std::is_void_v<FutureType>, yaclib::Unit, FutureType>;
  EXPECT_FALSE(f.Ready());
  std::move(p).Set(Value{});
  EXPECT_TRUE(f.Ready());
  auto result = std::move(f).Get();
  EXPECT_EQ(result.State(), yaclib::ResultState::Value);
  EXPECT_EQ(std::move(result).Ok(), Value{});
}

void AsyncGetResult(int num_threads) {
  static constexpr std::string_view kMessage = "Hello, world!";
  auto tp = yaclib::MakeThreadPool(num_threads);

  auto [f, p] = yaclib::MakeContract<std::string>();
  Submit(*tp, [p = std::move(p)]() mutable {
    std::move(p).Set(std::string{kMessage});
  });
  EXPECT_EQ(std::move(f).Get().Ok(), kMessage);
  tp->HardStop();
  tp->Wait();
}

TEST(JustWorks, Value) {
  ValueCheck<double>();
}

TEST(JustWorks, ErrorCode) {
  ErrorsCheck<double>(yaclib::StopError{yaclib::StopTag{}});
  ErrorsCheck<double, LikeErrorCode>(LikeErrorCode{});
}

TEST(JustWorks, Exception) {
  ErrorsCheck<double>(std::make_exception_ptr(std::runtime_error{""}));
}

TYPED_TEST(AsyncSuite, Run) {
  auto tp = yaclib::MakeThreadPool();
  bool called = false;
  auto f = INVOKE(*tp, [&] {
    called = true;
  });
  EXPECT_EQ(std::move(f).Get().Ok(), yaclib::Unit{});
  EXPECT_TRUE(called);
  tp->Stop();
  tp->Wait();
}

TYPED_TEST(AsyncSuite, RunException) {
  auto tp = yaclib::MakeThreadPool();
  auto result = INVOKE(*tp,
                       [] {
                         throw std::runtime_error{""};
                       })
                  .Then([] {
                    return 1;
                  })
                  .Get();
  EXPECT_THROW(std::move(result).Ok(), std::runtime_error);
  tp->Stop();
  tp->Wait();
}

TYPED_TEST(AsyncSuite, JustWorks) {
  auto tp = yaclib::MakeThreadPool();
  auto result = INVOKE(*tp,
                       [] {
                         return yaclib::Result<>{yaclib::StopTag{}};
                       })
                  .Then([] {
                    return 1;
                  })
                  .Get();
  EXPECT_THROW(std::move(result).Ok(), yaclib::ResultError<yaclib::StopError>);
  tp->Stop();
  tp->Wait();
}

TYPED_TEST(AsyncSuite, JustWorksVoidThen) {
  auto tp = yaclib::MakeThreadPool();
  auto f = INVOKE(*tp, [] {
           }).Then([] {
    return yaclib::Result<int>(1);
  });
  EXPECT_EQ(std::move(f).Get().Ok(), 1);
  tp->Stop();
  tp->Wait();
}

TEST(VoidJustWorks, Simple) {
  ValueCheck<void>();
}

TEST(VoidJustWorks, ErrorCode) {
  ErrorsCheck<void>(yaclib::StopError{yaclib::StopTag{}});
  ErrorsCheck<double, LikeErrorCode>(LikeErrorCode{});
}

TEST(VoidJustWorks, Exception) {
  ErrorsCheck<void>(std::make_exception_ptr(std::runtime_error{""}));
}

TEST(JustWorks, AsyncGetResult) {
  AsyncGetResult(1);
  AsyncGetResult(4);
}

TEST(JustWorks, AsyncRun) {
  auto tp = yaclib::MakeThreadPool(3);
  EXPECT_EQ(tp->Tag(), yaclib::IExecutor::Type::ThreadPool);

  auto good = [&] {
    EXPECT_EQ(&yaclib::CurrentThreadPool(), tp);
    return std::string{"Hello!"};
  };

  auto bad = [&]() -> int {
    EXPECT_EQ(&yaclib::CurrentThreadPool(), tp);
    throw std::logic_error("test");
  };
  auto f1 = yaclib::Run(*tp, good);
  auto f2 = yaclib::Run(*tp, bad);
  Wait(f1, f2);
  EXPECT_TRUE(f1.Ready());
  EXPECT_TRUE(f2.Ready());
  EXPECT_EQ(std::move(f1).Get().Ok(), "Hello!");
  EXPECT_THROW(std::move(f2).Get().Ok(), std::logic_error);

  tp->Stop();
  tp->Wait();
}

TEST(JustWorks, Promise) {
  auto tp = yaclib::MakeThreadPool(2);
  auto [f, p] = yaclib::MakeContract<>();

  int i = 0;
  auto g = std::move(f).Then(*tp, [&] {
    i = 1;
  });
  std::move(p).Set();
  Wait(g);
  EXPECT_EQ(i, 1);
  tp->Stop();
  tp->Wait();
}

TEST(Detach, AsyncSimple) {
  auto tp = yaclib::MakeThreadPool(1);

  auto [f, p] = yaclib::MakeContract<std::string>();

  bool called = false;

  std::move(f).Detach(*tp, [&](yaclib::Result<std::string> result) {
    EXPECT_EQ(&yaclib::CurrentThreadPool(), tp);
    EXPECT_EQ(std::move(result).Ok(), "Hello!");
    called = true;
  });

  EXPECT_FALSE(called);

  Submit(*tp, [p = std::move(p)]() mutable {
    std::move(p).Set("Hello!");
  });

  tp->SoftStop();
  tp->Wait();

  EXPECT_TRUE(called);
}

TEST(Detach, DetachOn) {
  auto tp = yaclib::MakeThreadPool(1);

  auto [f, p] = yaclib::MakeContract<int>();

  std::move(p).Set(17);

  yaclib_std::atomic<bool> called = false;

  auto callback = [&called](yaclib::Result<int> result) mutable {
    EXPECT_EQ(std::move(result).Ok(), 17);
    called.store(true);
  };

  // Schedule to thread pool immediately
  std::move(f).Detach(*tp, callback);

  tp->SoftStop();
  tp->Wait();

  EXPECT_TRUE(called);
}

TEST(Detach, DetachOn2) {
  auto tp1 = yaclib::MakeThreadPool(1);
  auto tp2 = yaclib::MakeThreadPool(1);

  auto [f, p] = yaclib::MakeContract<int>();

  yaclib_std::atomic<bool> called = false;

  auto callback = [&called](yaclib::Result<int> result) mutable {
    EXPECT_EQ(std::move(result).Ok(), 42);
    called.store(true);
  };

  std::move(f).Detach(*tp2, callback);

  Submit(*tp1, [p = std::move(p)]() mutable {
    std::move(p).Set(42);
  });

  tp1->SoftStop();
  tp1->Wait();
  tp2->SoftStop();
  tp2->Wait();

  EXPECT_TRUE(called);
}

TYPED_TEST(AsyncSuite, ThenThreadPool) {
  auto tp = yaclib::MakeThreadPool(4);
  auto compute = [] {
    return 42;
  };
  auto process = [](int v) -> int {
    return v + 1;
  };
  auto f1 = INVOKE(*tp, compute);
  auto f2 = std::move(f1).Then(process);
  EXPECT_EQ(std::move(f2).Get().Ok(), 43);
  tp->Stop();
  tp->Wait();
}

TEST(Simple, ThenOn) {
  auto tp = yaclib::MakeThreadPool(2);
  auto [f, p] = yaclib::MakeContract<>();
  auto g = std::move(f).Then(*tp, [&] {
    EXPECT_EQ(&yaclib::CurrentThreadPool(), tp);
    return 42;
  });
  // Launch
  std::move(p).Set();
  EXPECT_EQ(std::move(g).Get().Ok(), 42);

  tp->Stop();
  tp->Wait();
}

TEST(Simple, Stop) {
  auto tp = yaclib::MakeThreadPool(1);
  {
    yaclib::Promise<> p;
    {
      yaclib::Future<> f;
      {
        std::tie(f, p) = yaclib::MakeContract<>();
        auto g = std::move(f).Then(*tp, [] {
          return 42;
        });
      }
    }
  }
  {
    yaclib::FutureOn<int> g;
    {
      auto [f, p] = yaclib::MakeContract<>();
      g = std::move(f).Then(*tp, [] {
        return 42;
      });
    }
    EXPECT_THROW(std::move(g).Get().Ok(), yaclib::ResultError<yaclib::StopError>);
  }
  tp->SoftStop();
  tp->Wait();
}

TYPED_TEST(AsyncSuite, PipelineSimple) {
  auto tp = yaclib::MakeThreadPool(1);

  // Pipeline stages:

  auto first = [] {
    return 42;
  };

  auto second = [](int value) {
    return value * 2;
  };

  auto third = [](int value) {
    return value + 1;
  };

  auto f = INVOKE(*tp, first).Then(second).Then(third);

  EXPECT_EQ(std::move(f).Get().Ok(), 42 * 2 + 1);

  tp->Stop();
  tp->Wait();
}

class Calculator {
 public:
  Calculator(yaclib::IExecutor& e) : _executor{e} {
  }

  template <bool IsFuture = true>
  auto Increment(int value) {
    if constexpr (IsFuture) {
      return yaclib::Run(_executor, [value] {
        return value + 1;
      });
    } else {
      return yaclib::Schedule(_executor, [value] {
        return value + 1;
      });
    }
  }

  template <bool IsFuture = true>
  auto Double(int value) {
    if constexpr (IsFuture) {
      return yaclib::Run(_executor, [value] {
        return value * 2;
      });
    } else {
      return yaclib::Schedule(_executor, [value] {
        return value * 2;
      });
    }
  }

 private:
  yaclib::IExecutor& _executor;
};

TYPED_TEST(AsyncSuite, AsyncThen) {
  auto tp = yaclib::MakeThreadPool(4);

  Calculator calculator(*tp);

  auto pipeline = calculator.Increment<TestFixture::kIsFuture>(1)
                    .Then([&](int value) {
                      return calculator.Double(value);
                    })
                    .Then([&](int value) {
                      return calculator.Increment(value);
                    });
  EXPECT_EQ(std::move(pipeline).Get().Value(), 5);

  auto pipeline2 = calculator.Increment<TestFixture::kIsFuture>(1)
                     .ThenInline([&](int value) {
                       return calculator.Double(value);
                     })
                     .ThenInline([&](int value) {
                       return calculator.Increment(value);
                     });
  EXPECT_EQ(std::move(pipeline2).Get().Value(), 5);

  tp->SoftStop();
  tp->Wait();
}

TYPED_TEST(Error, Simple1) {
  using TestType = typename TestFixture::Type;
  static constexpr bool kIsError = !std::is_same_v<TestType, std::exception_ptr>;
  using ErrorType = std::conditional_t<kIsError, TestType, yaclib::StopError>;

  auto tp = yaclib::MakeThreadPool(4);
  // Pipeline stages:
  auto first = [] {
    return 1;
  };
  auto second = [](int v) {
    std::cout << v << " x 2" << std::endl;
    return v * 2;
  };
  auto third = [](int v) {
    std::cout << v << " + 1" << std::endl;
    return v + 1;
  };
  auto error_handler = [](TestType) -> int {
    return 42;
  };
  auto last = [&](int v) -> int {
    EXPECT_EQ(&yaclib::CurrentThreadPool(), tp);
    return v + 11;
  };

  auto f = yaclib::Run<ErrorType>(*tp, first).Then(second).Then(third).Then(error_handler).Then(last);
  EXPECT_EQ(std::move(f).Get().Ok(), 14);

  tp->Stop();
  tp->Wait();
}

TYPED_TEST(Error, Simple2) {
  using TestType = typename TestFixture::Type;
  static constexpr bool kIsError = !std::is_same_v<TestType, std::exception_ptr>;
  using ErrorType = std::conditional_t<kIsError, TestType, yaclib::StopError>;

  auto tp = yaclib::MakeThreadPool(1);
  // Pipeline stages:
  auto first = []() -> yaclib::Result<int, ErrorType> {
    if constexpr (kIsError) {
      return yaclib::StopTag{};
    } else {
      throw std::runtime_error{"first"};
    }
    return 0;
  };
  auto second = [](int v) {
    std::cout << v << " x 2" << std::endl;
    return v * 2;
  };
  auto third = [](int v) {
    std::cout << v << " + 1" << std::endl;
    return v + 1;
  };
  auto error_handler = [](TestType) -> int {
    return 42;
  };
  auto last = [&](int v) {
    EXPECT_EQ(&yaclib::CurrentThreadPool(), tp);
    return v + 11;
  };
  auto pipeline = yaclib::Run<ErrorType>(*tp, first).Then(second).Then(third).Then(error_handler).Then(last);
  EXPECT_EQ(std::move(pipeline).Get().Value(), 53);

  tp->Stop();
  tp->Wait();
}

TEST(Pipeline, Simple2) {
  auto tp1 = yaclib::MakeThreadPool(2);
  auto tp2 = yaclib::MakeThreadPool(3);

  auto [f, p] = yaclib::MakeContract<std::string>();

  auto make_stage = [](int index, yaclib::IThreadPoolPtr current_executor) {
    return [index, current_executor](std::string path) {
      EXPECT_EQ(&yaclib::CurrentThreadPool(), current_executor);
      std::cout << "At stage " << index << std::endl;
      return path + "->" + std::to_string(index);
    };
  };

  auto almost_there =
    std::move(f).Then(*tp1, make_stage(1, tp1)).Then(make_stage(2, tp1)).Then(*tp2, make_stage(3, tp2));

  std::move(p).Set("start");
  yaclib_std::this_thread::sleep_for(100ms);

  auto finally = std::move(almost_there).Then(make_stage(4, tp2)).Then(*tp1, make_stage(5, tp1));

  EXPECT_EQ(std::move(finally).Get().Ok(), "start->1->2->3->4->5");
  tp1->Stop();
  tp1->Wait();
  tp2->Stop();
  tp2->Wait();
}

TEST(Simple, MakePromiseContract) {
  yaclib::ManualExecutor e;
  EXPECT_EQ(e.Tag(), yaclib::IExecutor::Type::Custom);
  auto [f, p] = yaclib::MakeContract<int>();
  auto g = std::move(f).Then(e, [](int x) {
    return x + 1;
  });
  EXPECT_FALSE(g.Ready());
  std::move(p).Set(3);
  EXPECT_FALSE(g.Ready());
  e.Drain();
  ASSERT_TRUE(g.Ready());
  EXPECT_EQ(4, std::move(g).Get().Ok());
}

TYPED_TEST(AsyncSuite, ExceptionCallbackReturningValue) {
  auto tp = yaclib::MakeThreadPool(1);
  auto f = INVOKE(*tp, [] {
    return 1;
  });
  f = std::move(f)
        .Then([](yaclib::Result<int>) {
          throw std::runtime_error{""};
        })
        .Then([](yaclib::Result<> result) {
          EXPECT_EQ(result.State(), yaclib::ResultState::Exception);
          return 0;
        });
  EXPECT_EQ(std::move(f).Get().Ok(), 0);
  tp->Stop();
  tp->Wait();
}

TYPED_TEST(AsyncSuite, ExceptionCallbackReturningFuture) {
  auto tp = yaclib::MakeThreadPool(1);
  auto f = INVOKE(*tp, [] {
    return 1;
  });
  f = std::move(f)
        .Then([](int) -> yaclib::Future<> {
          throw std::runtime_error{""};
        })
        .Then([](yaclib::Result<> result) {
          EXPECT_EQ(result.State(), yaclib::ResultState::Exception);
          return 0;
        });
  EXPECT_EQ(std::move(f).Get().Ok(), 0);
  tp->Stop();
  tp->Wait();
}

TEST(Simple, SetAndGetRequiresOnlyMove) {
  struct MoveCtorOnly {
    explicit MoveCtorOnly(int id) : id_(id) {
    }

    MoveCtorOnly(MoveCtorOnly&&) = default;
    [[maybe_unused]] MoveCtorOnly& operator=(MoveCtorOnly&&) = default;
    MoveCtorOnly(const MoveCtorOnly&) = delete;
    MoveCtorOnly& operator=(MoveCtorOnly const&) = delete;
    int id_;
  };
  auto f = yaclib::MakeFuture<MoveCtorOnly>(MoveCtorOnly(42));
  EXPECT_TRUE(f.Ready());
  auto v = std::move(f).Get().Ok();
  EXPECT_EQ(v.id_, 42);
}

TYPED_TEST(AsyncSuite, Special) {
  using Type = typename TestFixture::Type;
  static_assert(!std::is_copy_constructible_v<Type>);
  static_assert(!std::is_copy_assignable_v<Type>);
  static_assert(std::is_move_constructible_v<Type>);
  static_assert(std::is_move_assignable_v<Type>);
}

TEST(Future, CheckReferenceWrapper) {
  int x = 5;
  auto [f, p] = yaclib::MakeContract<std::reference_wrapper<int>>();
  std::move(p).Set(std::ref(x));
}

TEST(Future, CheckConstGet) {
  auto [f, p] = yaclib::MakeContract<int>();
  auto ptr = std::as_const(f).Get();
  EXPECT_FALSE(f.Ready());
  EXPECT_EQ(ptr, nullptr);
  std::move(p).Set(5);
  ptr = std::as_const(f).Get();
  EXPECT_TRUE(f.Ready());
  EXPECT_NE(ptr, nullptr);
}

TYPED_TEST(AsyncSuite, DetachDrop) {
  auto tp = yaclib::MakeThreadPool(1);
  auto f = INVOKE(*tp, [&] {
    tp->Stop();
  });
  DETACH(f, [] {
    EXPECT_TRUE(false);
  });
  tp->Wait();
}

struct CDtorCounter {
  CDtorCounter& operator=(CDtorCounter&&) = delete;
  CDtorCounter& operator=(const CDtorCounter&) = delete;

  CDtorCounter(int& flag) : _flag{flag} {
    _flag += 1000;
  }

  CDtorCounter(CDtorCounter&& other) : _flag{other._flag} {
    _flag += 100;
  }

  CDtorCounter(const CDtorCounter& other) : _flag(other._flag) {
    _flag += 10;
  }

  ~CDtorCounter() {
    _flag += 1;
  }

 private:
  int& _flag;
};

TYPED_TEST(AsyncSuite, ExecutorDrop) {
  int flag = 0;

  INVOKE(MakeInline(yaclib::StopTag{}), [a = CDtorCounter{flag}] {
  }).Detach();

#if defined(_MSC_VER) && !defined(__clang__)
  EXPECT_EQ(flag, 1203);  // TODO(kononovk): fix windows
#else
  EXPECT_EQ(flag, 1102);
#endif
}

TYPED_TEST(AsyncSuite, ExecutorDrop2) {
  bool invoked_flags[3] = {false};

  INVOKE(MakeInline(yaclib::StopTag{}),
         [&] {
           invoked_flags[0] = true;
         })
    .Then([&](yaclib::StopError) {
      invoked_flags[1] = true;
      return yaclib::MakeFuture();
    })
    .Then([&](yaclib::StopError) {
      invoked_flags[2] = true;
    })
    .Detach();

  EXPECT_FALSE(invoked_flags[0]);
  EXPECT_TRUE(invoked_flags[1]);
  EXPECT_TRUE(invoked_flags[2]);
}

}  // namespace
}  // namespace test
