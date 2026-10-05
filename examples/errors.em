// Recoverable errors are values; unrecoverable ones stop with a trace.
enum Result { Ok(Int), Err(String) }

fn safe_div(a: Int, b: Int) -> Result {
    if b == 0 {
        return Result::Err("division by zero");
    }
    return Result::Ok(a / b);
}

fn main() {
    for b in [2, 0] {
        match safe_div(10, b) {
            Result::Ok(v) => {
                print("ok: " + str(v));
            },
            Result::Err(e) => {
                print("error: " + e);
            },
        }
    }
    let xs = [1, 2, 3];
    print(xs[3]);
}
