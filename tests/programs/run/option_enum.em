enum Parsed { Ok(Int), Err(String) }

fn parse_digit(s: String) -> Parsed {
    if len(s) != 1 { return Parsed::Err("expected one character"); }
    return match s {
        "0" => Parsed::Ok(0),
        "1" => Parsed::Ok(1),
        "2" => Parsed::Ok(2),
        _ => Parsed::Err("not a digit: " + s),
    };
}

fn main() {
    for s in ["1", "x", "22", "2"] {
        match parse_digit(s) {
            Parsed::Ok(v) => { print(v * 100); },
            Parsed::Err(msg) => { print("error: " + msg); },
        }
    }
}
