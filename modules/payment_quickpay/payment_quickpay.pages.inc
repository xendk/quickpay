<?php

/**
 * @file
 * Contains page callbacks.
 */

/**
 * Redirect form for Quickpay payments.
 */
function payment_quickpay_form($form, &$form_state, Payment $payment) {
  try {
    $qp = new Quickpay($payment->method->controller_data);
  }
  catch (Exception $e) {
    drupal_set_message(t("Quickpay payment haven't been properly configured. Consult the log for details."), 'error');
    watchdog('payment_quickpay', 'Quickpay payment failed, message: @message', array('@message' => $e->getMessage()), WATCHDOG_EMERGENCY);
    return array();
  }

  $token = drupal_get_token($payment->pid);
  $options = array(
    'continue_url'  => url('payment/quickpay/return/' . $payment->pid . '/' . $token . '/continue', array('absolute' => TRUE)),
    'cancel_url'    => url('payment/quickpay/return/' . $payment->pid . '/' . $token . '/cancel', array('absolute' => TRUE)),
  );

  // Allow the payment to set autocapture.
  if (!empty($payment->context_data['autocapture'])) {
    $options['autocapture'] = TRUE;
  }

  $form = $qp->form($payment->pid, $payment->totalAmount(TRUE), $payment->currency_code, 'payment_quickpay', $options);

  return $form;
}

/**
 * Page callback for returning from Quickpay.
 *
 * @param $payment
 *  The payment entity.
 * @param $action
 *  One of either 'continue' or 'cancel'.
 */
function payment_quickpay_return_page(Payment $payment, $action) {
  switch ($action) {
    case 'cancel':
      // If the payment was cancelled, set the status.
      $payment->setStatus(new PaymentStatusItem(PAYMENT_STATUS_CANCELLED));
      break;
    case 'continue':
      // If the payment was successful, the Quickpay callback will have updated
      // the Payment entity: see payment_quickpay_quickpay_callback().
      break;
    default:
      throw new Exception("Invalid action parameter.");
  }

  // Finish the payment. This saves the Payment, and calls the Payment context's
  // finish callback. That should take care of showing a message to the user and
  // redirecting then to a suitable page.
  $payment->finish();
}

/**
 * Form builder for the payment capture tab.
 */
function payment_quickpay_capture_form($form, &$form_state, Payment $payment) {
  if (!empty($payment->context_data['autocapture'])) {
    $form['autocapture'] = array(
      '#markup' => t("This payment used autocapture and is already complete."),
    );
    return $form;
  }

  $status = $payment->getStatus()->status;
  if ($status != PAYMENT_STATUS_PENDING) {
    $form['autocapture'] = array(
      '#markup' => t("No funds pending capture."),
    );
    return $form;
  }

  $form_state['payment'] = $payment;

  $form['capture_amount'] = array(
    '#type' => 'textfield',
    '#title' => t('Capture amount'),
    '#default_value' => $payment->totalAmount(TRUE),
    '#field_suffix' => $payment->currency_code,
    '#size' => 5,
    // TODO: remove this once we figure out how to record the captured amount in
    // a payment entity.
    '#disabled' => TRUE,
    '#description' => t("Only the full amount may be captured."),
  );
  $form['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Capture'),
  );

  return $form;
}

/**
 * Form validation handler for payment capture.
 */
function payment_quickpay_capture_form_validate($form, &$form_state) {
  $payment = $form_state['payment'];
  $amount = $form_state['values']['capture_amount'];

  // Ensure a positive numeric amount has been entered for capture.
  if (!is_numeric($amount) || $amount <= 0) {
    form_set_error('capture_amount', t('You must specify a positive numeric amount to capture.'));
  }

  // Ensure the amount is less than or equal to the authorization amount.
  if ($amount > $payment->totalAmount(TRUE)) {
    form_set_error('capture_amount', t('You cannot capture more than the origianl amount authorized through QuickPay.'));
  }
}

/**
 * Form submit handler for payment capture.
 */
function payment_quickpay_capture_form_submit($form, &$form_state) {
  $payment = $form_state['payment'];
  $amount = $form_state['values']['capture_amount'];

  $qp = new Quickpay($payment->method->controller_data);
  $txn = $qp->transaction($payment->context_data['remote_id']);

  $txn->capture($amount);

  if ($txn->success()) {
    drupal_set_message(t('Payment was succesfully captured.'));
    $payment_status = PAYMENT_STATUS_SUCCESS;
  }
  else {
    drupal_set_message(t('Payment capture failed, message: "@message".', array('@message' => $txn->lastMessage())), 'error');
    $payment_status = PAYMENT_STATUS_FAILED;
  }

  $payment->setStatus(new PaymentStatusItem($payment_status));
  entity_save('payment', $payment);
}
