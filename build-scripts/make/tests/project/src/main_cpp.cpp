// Copyright (c) Meta Platforms, Inc. and affiliates.

extern "C" int a_value(void);
extern "C" int b_value(void);

int main()
{
    return a_value() + b_value() == 3 ? 0 : 1;
}
