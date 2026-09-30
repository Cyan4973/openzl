// Copyright (c) Meta Platforms, Inc. and affiliates.

int a_value(void);
int b_value(void);

int main(void)
{
    return a_value() + b_value() == 3 ? 0 : 1;
}
