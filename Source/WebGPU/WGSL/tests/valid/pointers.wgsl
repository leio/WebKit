// RUN: %metal-compile main

fn f1(x: ptr<function, i32>) -> i32
{
    return *x;
}

fn f2(x: ptr<private, i32>) -> i32
{
    return *x;
}

fn f3(x: vec3<f32>) { }

var<private> global: i32;

struct S { x: vec3<f32> }
@group(0) @binding(0) var<storage, read_write> global2 : S;

fn testSimplePointerRewriting()
{
    let x = &global2;
    f3((*x).x);
}

fn testIndirectPointerRewriting()
{
    let x = &global2;
    let y = x;
    let z = y;
    f3((*z).x);
}

fn testPhonyPointerElimination()
{
    _ = &global2;
}

fn testShadowedGlobalRewriting()
{
    let x = &global2;
    let global2 = 0;
    f3((*x).x);
}

fn testShadowedLocalRewriting()
{
    var x: S;
    let y = &x;
    {
        let x = 0;
        f3((*y).x);
    }
}

@compute @workgroup_size(1)
fn main()
{
    var local: i32;

    _ = &global2;

    let x = &global2;
    let y = x;

    f1(&local);
    f2(&global);

    testSimplePointerRewriting();
    testIndirectPointerRewriting();
    testPhonyPointerElimination();
    testShadowedGlobalRewriting();
    testShadowedLocalRewriting();
}
