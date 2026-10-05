enum E { A, B, C }
fn main() {
    let e = E::A;
    let n = match e { E::A => 1, E::B => 2 };
}
