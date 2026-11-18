// Values created fresh (by f) rather than moved out of a just.
{
  auto make = [](int i) { return counted(std::string(std::size_t(i), 'x')); };
  {
    auto s = k::then(k::just(3), make);
    moves = 0;
    auto r = k::sync_wait(std::move(s));
    CHECK(r && r->s.size() == 3);
    std::printf("%s fresh: then(just) | sync_wait: moves=%d\n", COL, moves);
  }
  {
    auto s = k::let_value(k::just(1), [make](int& i) { return k::then(k::just(i + 1), make); });
    moves = 0;
    auto r = k::sync_wait(std::move(s));
    CHECK(r && r->s.size() == 2);
    std::printf("%s fresh: let_value(.., then) | sync_wait: moves=%d\n", COL, moves);
  }
  {
    auto s = k::then(k::when_all(k::then(k::just(1), make), k::then(k::just(2), make)), SUM);
    moves = 0;
    auto r = k::sync_wait(std::move(s));
    CHECK(r && *r == 3);
    std::printf("%s fresh: when_all(then, then) | then: moves=%d\n", COL, moves);
  }
}
