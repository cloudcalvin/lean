open tactic nat expr option

definition succ_eq_add [defeq] (n : nat) : succ n = n + 1 :=
rfl

example (a b : nat) : a = b → succ (succ a) = succ (b + 1) :=
by do intro "Heq",
      t  ← target,
      trace_state,
      t' ← defeq_simp t,
      change t',
      trace "---- after change ----",
      trace_state,
      subst "a",
      t ← target,
      match is_eq t with
      | some (lhs, rhs) := do pr ← mk_app ("eq" <.> "refl") [lhs], exact pr
      | none            := failed
      end