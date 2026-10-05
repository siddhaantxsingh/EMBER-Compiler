// Category: enums and match (a tiny stack-machine interpreter).
enum Op { Push(Int), Add, Mul, Dup, Swap, Pop, JumpIfPos(Int), Dec }

fn run(prog: [Op], start: Int) -> Int {
    var stack = [start];
    var pc = 0;
    var steps = 0;
    while pc < len(prog) {
        steps += 1;
        match prog[pc] {
            Op::Push(v) => { push(stack, v); },
            Op::Add => { let b = pop(stack); let a = pop(stack); push(stack, a + b); },
            Op::Mul => { let b = pop(stack); let a = pop(stack); push(stack, a * b % 1000000007); },
            Op::Dup => { let a = pop(stack); push(stack, a); push(stack, a); },
            Op::Swap => { let b = pop(stack); let a = pop(stack); push(stack, b); push(stack, a); },
            Op::Pop => { pop(stack); },
            Op::JumpIfPos(t) => {
                let a = pop(stack);
                push(stack, a);
                if a > 0 { pc = t - 1; }
            },
            Op::Dec => { let a = pop(stack); push(stack, a - 1); },
        }
        pc += 1;
    }
    return steps;
}

fn main() {
    // acc = acc * 3 + 7 (mod p), repeated `start` times
    // stack: [acc, counter]; loop body starts at index 2
    let prog = [Op::Push(1), Op::Swap, Op::Swap, Op::Push(3), Op::Mul, Op::Push(7), Op::Add, Op::Swap, Op::Dec,
                Op::JumpIfPos(2), Op::Pop];
    print(run(prog, 1000000));
}
