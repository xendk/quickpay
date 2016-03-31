<?php

/**
 * This file contains classes for the QuickPay Payment module.
 */

/**
 * QuickPay payment method controller.
 */
class QuickpayPaymentMethodController extends PaymentMethodController {

  public $payment_method_configuration_form_elements_callback = 'payment_quickpay_method_configuration';

  function __construct() {
    $this->title = t('Quickpay');
  }

  /**
   * Implements PaymentMethodController::execute().
   */
  function execute(Payment $payment) {
    // Start a session for anonymous users, otherwise drupal_valid_token() won't
    // work
    global $user;
    if (empty($user->uid)) {
      drupal_session_start();
    }

    drupal_goto('payment/quickpay/form/' . $payment->pid . '/' . drupal_get_token($payment->pid));
  }

}
